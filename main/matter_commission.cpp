/* Pairing a Matter device, and finding the lamps on it.
 *
 * On the network only, from a code another app gives out: somebody picks "Turn
 * on pairing mode" in Apple Home (multi-admin), and types the code into our page.
 * BLE stays off. The device is found over DNS-SD, then commissioned the way
 * CHIP's own DeviceCommissioner does it, on the Server's objects instead of a
 * controller stack's (see matter_fabric.cpp for why):
 *
 *   PASE (as initiator) -> ArmFailSafe -> CSRRequest -> AddTrustedRootCertificate
 *   -> AddNOC -> CASE on our fabric -> CommissioningComplete -> endpoint discovery
 *
 * Each step runs in the previous one's callback, on the CHIP thread. The one step
 * that blocks - the DNS-SD browse - runs in a task of its own: mdns_query_ptr
 * waits for answers, and the CHIP thread must never wait.
 *
 * Device attestation is not checked (a decision, not an omission): a lamp whose
 * certificate was not verified is paired, and shown as such.
 *
 * One pairing at a time; the page polls its progress from /api/matter. */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "mdns.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/ConcreteAttributePath.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/server/Server.h>
#include <controller/InvokeInteraction.h>
#include <credentials/CHIPCert.h>
#include <lib/core/TLV.h>
#include <messaging/ReliableMessageProtocolConfig.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/PASESession.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>

#include <esp_matter.h>

#include "device_db.h"
#include "heap_watch.h"
#include "matter_priv.h"
#include "mlight_db.h"
#include "settings.h"

static const char *TAG = "mpair";

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

/* >>> host-test region: mctl-disc <<< */
/* Which endpoints of a paired Matter device are lamps, and what each one can do:
 * a pure function of what the device's Descriptor, Color Control and (Bridged)
 * Basic Information clusters said, so test/test_mctl_disc.c can drive it. Plain C
 * inside a C++ file, as the `matter` region is. */

/* Device types that are a light, from the Matter device library. Plug-in units
 * count: a dimmable plug with a lamp in it is a lamp to everybody here. */
#define MCTL_DT_ON_OFF_LIGHT         0x0100u
#define MCTL_DT_DIMMABLE_LIGHT       0x0101u
#define MCTL_DT_ON_OFF_PLUG          0x010Au
#define MCTL_DT_DIMMABLE_PLUG        0x010Bu
#define MCTL_DT_COLOR_TEMP_LIGHT     0x010Cu
#define MCTL_DT_EXTENDED_COLOR_LIGHT 0x010Du
/* An endpoint that stands for a device behind a hub. */
#define MCTL_DT_BRIDGED_NODE         0x0013u

/* Color Control's FeatureMap. */
#define MCTL_CC_HUE_SAT (1u << 0)
#define MCTL_CC_XY      (1u << 3)
#define MCTL_CC_CT      (1u << 4)

#define MCTL_DISC_STR 33

typedef struct {
    uint16_t ep;
    bool     light;           /* a light device type was listed */
    bool     bridged;         /* ...on an endpoint standing for a device behind a hub */
    bool     onoff, level, color;   /* server clusters */
    uint32_t cc_features;     /* Color Control FeatureMap, 0 until read */
    uint16_t ct_min, ct_max;  /* ColorTempPhysicalMin/MaxMireds, 0 until read */
    /* Bridged Device Basic Information: what a hub says about the lamp. */
    char     label[MCTL_DISC_STR];
    char     vendor[MCTL_DISC_STR];
    char     product[MCTL_DISC_STR];
    uint16_t vendor_id, product_id;
} mctl_disc_ep_t;

/* Basic Information on endpoint 0: what the device says about itself. */
typedef struct {
    char     label[MCTL_DISC_STR];
    char     vendor[MCTL_DISC_STR];
    char     product[MCTL_DISC_STR];
    uint16_t vendor_id, product_id;
} mctl_disc_node_t;

static bool mctl_disc_light_type(uint32_t type)
{
    switch (type) {
    case MCTL_DT_ON_OFF_LIGHT:
    case MCTL_DT_DIMMABLE_LIGHT:
    case MCTL_DT_ON_OFF_PLUG:
    case MCTL_DT_DIMMABLE_PLUG:
    case MCTL_DT_COLOR_TEMP_LIGHT:
    case MCTL_DT_EXTENDED_COLOR_LIGHT:
        return true;
    default:
        return false;
    }
}

/* A lamp is an endpoint that says it is a light and can be switched. A device
 * type alone is not enough: an endpoint that lists one without an On/Off server
 * has nothing to send a command to. */
static bool mctl_disc_is_lamp(const mctl_disc_ep_t *e)
{
    return e->light && e->onoff;
}

/* What a lamp can do, in the feature bits every other light here carries. Colour
 * comes from Color Control's FeatureMap, never from the device type: an Extended
 * Color Light may lack colour temperature, and the page must not offer what the
 * lamp would refuse. */
static uint16_t mctl_disc_features(const mctl_disc_ep_t *e)
{
    uint16_t f = ZBC_FEAT_ONOFF;
    if (e->level) {
        f |= ZBC_FEAT_LEVEL;
    }
    if (e->color) {
        if (e->cc_features & MCTL_CC_HUE_SAT) {
            f |= ZBC_FEAT_COLOR_HS;
        }
        if (e->cc_features & MCTL_CC_XY) {
            f |= ZBC_FEAT_COLOR_XY;
        }
        if (e->cc_features & MCTL_CC_CT) {
            f |= ZBC_FEAT_COLOR_TEMP;
        }
    }
    return f;
}

/* Copies a name without splitting a UTF-8 character at the cut. */
static void mctl_disc_copy(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) {
            n--;              /* back off to the start of the cut character */
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

/* What the lamp is called here to begin with. Behind a hub, what the hub calls it
 * - "Kitchen ceiling", which somebody typed into the hub's app - and failing that
 * its product. On a device of its own, the name the device carries, or its
 * product, numbered when the device has more than one lamp (a dual plug). */
static void mctl_disc_name(const mctl_disc_ep_t *e, const mctl_disc_node_t *n, int lamps_on_node,
                           char *out, size_t cap)
{
    const char *base;
    if (e->bridged) {
        base = e->label[0] ? e->label : (e->product[0] ? e->product : n->product);
    } else {
        base = n->label[0] ? n->label : n->product;
    }
    if (!base[0]) {
        base = "Matter lamp";
    }
    /* A name a person gave the lamp in the hub's app is used as it is. */
    if (base == e->label) {
        mctl_disc_copy(out, cap, base);
        return;
    }
    if (lamps_on_node > 1) {
        char tmp[MCTL_DISC_STR + 8];
        snprintf(tmp, sizeof(tmp), "%s %u", base, (unsigned)e->ep);
        mctl_disc_copy(out, cap, tmp);
        return;
    }
    mctl_disc_copy(out, cap, base);
}

/* Maker and model: the lamp's own behind a hub, where the hub says them, and the
 * device's otherwise - a Hue Bridge is not the lamp. */
static void mctl_disc_identity(const mctl_disc_ep_t *e, const mctl_disc_node_t *n, const char **vendor,
                               const char **product, uint16_t *vendor_id, uint16_t *product_id)
{
    const bool own = e->bridged && (e->vendor[0] || e->product[0]);
    *vendor     = own && e->vendor[0] ? e->vendor : n->vendor;
    *product    = own && e->product[0] ? e->product : n->product;
    *vendor_id  = e->bridged && e->vendor_id ? e->vendor_id : n->vendor_id;
    *product_id = e->bridged && e->product_id ? e->product_id : n->product_id;
}
/* >>> end host-test region: mctl-disc <<< */

namespace {

/* ------------------------------------------------------------ progress */

enum step_t : uint8_t {
    ST_IDLE = 0,
    ST_FIND,        /* DNS-SD browse for the device in pairing mode */
    ST_PASE,
    ST_ARM,         /* ArmFailSafe */
    ST_CSR,
    ST_ROOT,        /* AddTrustedRootCertificate */
    ST_NOC,         /* AddNOC */
    ST_CASE,
    ST_COMPLETE,    /* CommissioningComplete */
    ST_READ,        /* endpoint discovery */
    ST_DONE,
    ST_FAILED,
};

const char *const STEP_NAMES[] = { "idle", "find", "pase", "arm", "csr", "root", "noc",
                                   "case", "complete", "read", "done", "failed" };
static_assert(sizeof(STEP_NAMES) / sizeof(STEP_NAMES[0]) == MCTL_PAIR_STEPS, "a step without a name");

/* How long a pairing may take, all steps together. PASE with a slow PBKDF on the
 * device, and a Thread device's first operational lookup, each take seconds. */
constexpr uint32_t kPairTimeoutS  = 120;
/* The lamp's fail-safe. Long enough for every step after PASE; should the box die
 * halfway, the lamp forgets the half-done pairing by itself after this long. */
constexpr uint16_t kFailSafeS     = 90;
/* How long the browse keeps looking. Apple's pairing mode advertises within a
 * second or two; a Thread device's record comes through the border router's
 * advertising proxy, which can take longer. */
constexpr int      kFindAttempts  = 5;
constexpr uint32_t kFindWindowMs  = 3000;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Shared with other tasks, under s_mux. */
struct {
    uint8_t  step;
    uint8_t  state;              /* mctl_pair_state_t */
    char     error[96];
    uint64_t node;
    uint8_t  lamps;
    int64_t  started_us;
    uint32_t gen;                /* bumps per pairing, so a late callback finds it gone */
    /* The code, and what the browse found. */
    uint32_t pin;
    bool     short_disc;
    uint16_t disc;
    bool     found;
    uint32_t heap_low[MCTL_PAIR_STEPS];
    uint8_t  addr[16];           /* IPv6, or IPv4-mapped */
    uint16_t port;
    void    *netif;              /* lwIP netif of the answer, for a link-local address */
} s_p;

/* CHIP thread only. */
PASESession  *s_pase;
SessionHolder s_pase_session;
SessionHolder s_case_session;
/* CSRRequest's nonce is 32 bytes (Controller::kCSRNonceLength, in a header this
 * build does not otherwise pull in). */
uint8_t       s_nonce[32];
uint8_t       s_lamp_fabric;
/* The lamp's certificate, between the CSR step that makes it and the AddNOC that
 * sends it. Static rather than on the heap: one pairing at a time, 400 bytes. */
uint8_t       s_noc[Credentials::kMaxCHIPCertLength];
size_t        s_noc_len;
/* The key from the lamp's CSR, between the answer that carries it and the work
 * item that signs for it. */
Crypto::P256PublicKey s_lamp_key;

void case_connected(void *, Messaging::ExchangeManager &, const SessionHandle & session);
void case_failed(void *, const ScopedNodeId &, CHIP_ERROR err);
Callback::Callback<OnDeviceConnected>         s_on_case(case_connected, nullptr);
Callback::Callback<OnDeviceConnectionFailure> s_on_case_fail(case_failed, nullptr);

/* Caller holds s_mux. The step that is ending gets the heap's low so far. */
void note_heap_locked()
{
    if (s_p.step < MCTL_PAIR_STEPS) {
        s_p.heap_low[s_p.step] = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    }
}

void set_step(step_t st)
{
    portENTER_CRITICAL(&s_mux);
    note_heap_locked();
    s_p.step = st;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Pairing: %s", STEP_NAMES[st]);
}

bool pairing_live(uint32_t gen)
{
    portENTER_CRITICAL(&s_mux);
    const bool live = s_p.gen == gen && s_p.state == MCTL_PAIR_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    return live;
}

uint32_t current_gen()
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t g = s_p.gen;
    portEXIT_CRITICAL(&s_mux);
    return g;
}

void timeout_cb(System::Layer *, void *);

/* Releases everything the pairing holds. Deferred to its own event, never run
 * inside a PASE or invoke callback: the object calling us is still on the stack. */
void cleanup_work(intptr_t)
{
    if (s_pase) {
        Platform::Delete(s_pase);
        s_pase = nullptr;
    }
    if (s_pase_session) {
        /* A PASE session outliving its pairing is a door left open. */
        s_pase_session->AsSecureSession()->MarkForEviction();
        s_pase_session.Release();
    }
    s_case_session.Release();
    /* A CASE attempt still running would otherwise answer the next pairing. */
    s_on_case.Cancel();
    s_on_case_fail.Cancel();
    DeviceLayer::SystemLayer().CancelTimer(timeout_cb, nullptr);
}

void finish(mctl_pair_state_t state, const char *error)
{
    portENTER_CRITICAL(&s_mux);
    if (s_p.state != MCTL_PAIR_RUNNING) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    note_heap_locked();
    s_p.state = (uint8_t)state;
    s_p.step  = state == MCTL_PAIR_DONE ? ST_DONE : ST_FAILED;
    strlcpy(s_p.error, error ? error : "", sizeof(s_p.error));
    s_p.gen++;
    portEXIT_CRITICAL(&s_mux);
    heap_watch_mark("mpair_end", (uint32_t)state);
    if (state == MCTL_PAIR_DONE) {
        ESP_LOGI(TAG, "Pairing done");
    } else {
        ESP_LOGW(TAG, "Pairing failed: %s", error);
    }
    if (DeviceLayer::PlatformMgr().ScheduleWork(cleanup_work, 0) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Could not schedule the pairing's clean-up");
    }
}

void fail(const char *what, CHIP_ERROR err)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s (%" CHIP_ERROR_FORMAT ")", what, err.Format());
    finish(MCTL_PAIR_FAILED, msg);
}

void timeout_cb(System::Layer *, void *)
{
    fail("the device stopped answering", CHIP_ERROR_TIMEOUT);
}

/* One command to the device, its answer handed to `ok` - unless the pairing has
 * ended meanwhile, in which case nobody is listening any more. */
template <typename Req, typename OnOk>
void invoke(const SessionHandle & session, const Req & req, const char *what, OnOk ok)
{
    const uint32_t gen = current_gen();
    CHIP_ERROR err = Controller::InvokeCommandRequest(
        &Server::GetInstance().GetExchangeManager(), session, kRootEndpointId, req,
        [gen, ok](const ConcreteCommandPath &, const StatusIB &, const typename Req::ResponseType & rsp) {
            if (pairing_live(gen)) {
                ok(rsp);
            }
        },
        [gen, what](CHIP_ERROR e) {
            if (pairing_live(gen)) {
                fail(what, e);
            }
        });
    if (err != CHIP_NO_ERROR) {
        fail(what, err);
    }
}

/* ------------------------------------------------ the commissioning steps */

void on_complete(const GeneralCommissioning::Commands::CommissioningCompleteResponse::DecodableType & rsp);
void on_discovered(NodeId node, CHIP_ERROR err, int lamps);

/* Our name in the lamp's own list of who controls it - what Apple Home shows
 * under the lamp's connected services. A nicety: nothing waits for it. */
void send_label(const SessionHandle & session)
{
    char label[33];
    strlcpy(label, settings_name(), sizeof(label));
    OperationalCredentials::Commands::UpdateFabricLabel::Type req;
    req.label = CharSpan::fromCharString(label);
    CHIP_ERROR err = Controller::InvokeCommandRequest(
        &Server::GetInstance().GetExchangeManager(), session, kRootEndpointId, req,
        [](const ConcreteCommandPath &, const StatusIB &,
           const OperationalCredentials::Commands::NOCResponse::DecodableType &) {},
        [](CHIP_ERROR e) { ESP_LOGW(TAG, "The lamp did not take our label: %" CHIP_ERROR_FORMAT, e.Format()); });
    (void)err;
}

void case_connected(void *, Messaging::ExchangeManager &, const SessionHandle & session)
{
    if (!pairing_live(current_gen())) {
        return;
    }
    s_case_session.Grab(session);
    set_step(ST_COMPLETE);
    invoke(session, GeneralCommissioning::Commands::CommissioningComplete::Type(), "CommissioningComplete failed",
           on_complete);
}

void case_failed(void *, const ScopedNodeId &, CHIP_ERROR err)
{
    if (pairing_live(current_gen())) {
        fail("the device could not be reached on our fabric", err);
    }
}


void on_complete(const GeneralCommissioning::Commands::CommissioningCompleteResponse::DecodableType & rsp)
{
    if (rsp.errorCode != GeneralCommissioning::CommissioningErrorEnum::kOk) {
        char msg[64];
        snprintf(msg, sizeof(msg), "the device refused CommissioningComplete (%u)", (unsigned)rsp.errorCode);
        finish(MCTL_PAIR_FAILED, msg);
        return;
    }
    /* Paired: the device is ours now, whatever happens next. The PASE session
     * has done its job. */
    if (s_pase_session) {
        s_pase_session->AsSecureSession()->MarkForEviction();
        s_pase_session.Release();
    }
    NodeId node;
    portENTER_CRITICAL(&s_mux);
    node = s_p.node;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Commissioned node 0x%016" PRIx64 "; its fabric index for us is %u", node, s_lamp_fabric);

    auto session = s_case_session.Get();
    if (!session.HasValue()) {
        finish(MCTL_PAIR_FAILED, "the session closed before the lamps were read");
        return;
    }
    send_label(session.Value());
    set_step(ST_READ);
    CHIP_ERROR err = mctl::discover_start(node, s_lamp_fabric, session.Value(), on_discovered);
    if (err != CHIP_NO_ERROR) {
        fail("could not read the device's endpoints", err);
    }
}

void on_noc(const OperationalCredentials::Commands::NOCResponse::DecodableType & rsp)
{
    if (rsp.statusCode != OperationalCredentials::NodeOperationalCertStatusEnum::kOk) {
        char msg[80];
        snprintf(msg, sizeof(msg), "the device refused our certificate (status %u%s)", (unsigned)rsp.statusCode,
                 rsp.statusCode == OperationalCredentials::NodeOperationalCertStatusEnum::kTableFull
                     ? ": its fabric table is full" : "");
        finish(MCTL_PAIR_FAILED, msg);
        return;
    }
    s_lamp_fabric = rsp.fabricIndex.HasValue() ? rsp.fabricIndex.Value() : 0;
    NodeId node;
    portENTER_CRITICAL(&s_mux);
    node = s_p.node;
    portEXIT_CRITICAL(&s_mux);
    /* The device now advertises itself on our fabric; CASE finds it by DNS-SD,
     * which is also how every later session and resubscription will. */
    set_step(ST_CASE);
    Server::GetInstance().GetCASESessionManager()->FindOrEstablishSession(ScopedNodeId(node, mctl::fabric_index()),
                                                                          &s_on_case, &s_on_case_fail);
}

void on_root(const DataModel::NullObjectType &)
{
    auto pase = s_pase_session.Get();
    if (!pase.HasValue()) {
        finish(MCTL_PAIR_FAILED, "the pairing session closed");
        return;
    }
    /* The certificate issued at the CSR step, not a second one for the same key. */
    set_step(ST_NOC);
    OperationalCredentials::Commands::AddNOC::Type req;
    req.NOCValue         = ByteSpan(s_noc, s_noc_len);
    req.IPKValue         = mctl::fabric_ipk();
    /* We administer the lamp: its ACL gets an entry for our node. */
    req.caseAdminSubject = mctl::fabric_own_node();
    req.adminVendorId    = static_cast<VendorId>(mctl::fabric_vendor_id());
    invoke(pase.Value(), req, "AddNOC failed", on_noc);
}

void issue_work(intptr_t gen);

void on_csr(const OperationalCredentials::Commands::CSRResponse::DecodableType & rsp)
{
    /* NOCSRElements: a structure holding the CSR (tag 1) and our nonce (tag 2). */
    TLV::TLVReader reader;
    reader.Init(rsp.NOCSRElements);
    TLV::TLVType outer;
    ByteSpan     csr, nonce;
    CHIP_ERROR   err = reader.Next(TLV::kTLVType_Structure, TLV::AnonymousTag());
    if (err == CHIP_NO_ERROR) {
        err = reader.EnterContainer(outer);
    }
    while (err == CHIP_NO_ERROR && (err = reader.Next()) == CHIP_NO_ERROR) {
        if (reader.GetTag() == TLV::ContextTag(1)) {
            err = reader.Get(csr);
        } else if (reader.GetTag() == TLV::ContextTag(2)) {
            err = reader.Get(nonce);
        }
    }
    if (err == CHIP_END_OF_TLV) {
        err = CHIP_NO_ERROR;
    }
    if (err != CHIP_NO_ERROR || csr.empty()) {
        fail("the device's CSR did not parse", err != CHIP_NO_ERROR ? err : CHIP_ERROR_INVALID_TLV_ELEMENT);
        return;
    }
    if (!nonce.data_equal(ByteSpan(s_nonce))) {
        finish(MCTL_PAIR_FAILED, "the device's CSR answered another request");
        return;
    }
    err = Crypto::VerifyCertificateSigningRequest(csr.data(), csr.size(), s_lamp_key);
    if (err != CHIP_NO_ERROR) {
        fail("the device's CSR is not valid", err);
        return;
    }
    /* Signing is an ECDSA operation under an X.509 writer, a few KB of stack,
     * and this callback runs at the bottom of CHIP's receive path on the CHIP
     * task's 8 KB. It gets an event of its own. */
    if (DeviceLayer::PlatformMgr().ScheduleWork(issue_work, (intptr_t)current_gen()) != CHIP_NO_ERROR) {
        finish(MCTL_PAIR_FAILED, "the Matter task did not take the signing");
    }
}

/* Signs the lamp's certificate and sends our root: the step after the CSR. */
void issue_work(intptr_t gen)
{
    if (!pairing_live((uint32_t)gen)) {
        return;
    }
    const NodeId node = mctl::fabric_new_node_id();
    MutableByteSpan nocSpan(s_noc);
    CHIP_ERROR err = node == kUndefinedNodeId ? CHIP_ERROR_INTERNAL : mctl::fabric_issue_noc(s_lamp_key, node, nocSpan);
    if (err != CHIP_NO_ERROR) {
        fail("could not sign the device's certificate", err);
        return;
    }
    s_noc_len = nocSpan.size();
    portENTER_CRITICAL(&s_mux);
    s_p.node = node;
    portEXIT_CRITICAL(&s_mux);

    uint8_t         rcac[Credentials::kMaxCHIPCertLength];
    MutableByteSpan rcacSpan(rcac);
    err = mctl::fabric_root_cert(rcacSpan);
    auto pase = s_pase_session.Get();
    if (err != CHIP_NO_ERROR || !pase.HasValue()) {
        fail("our root certificate is missing", err != CHIP_NO_ERROR ? err : CHIP_ERROR_INCORRECT_STATE);
        return;
    }
    set_step(ST_ROOT);
    OperationalCredentials::Commands::AddTrustedRootCertificate::Type req;
    req.rootCACertificate = rcacSpan;
    /* The request is encoded before this returns, so the stack buffer may go. */
    invoke(pase.Value(), req, "AddTrustedRootCertificate failed", on_root);
}

void on_armed(const GeneralCommissioning::Commands::ArmFailSafeResponse::DecodableType & rsp)
{
    if (rsp.errorCode != GeneralCommissioning::CommissioningErrorEnum::kOk) {
        char msg[64];
        snprintf(msg, sizeof(msg), "the device would not arm its fail-safe (%u)", (unsigned)rsp.errorCode);
        finish(MCTL_PAIR_FAILED, msg);
        return;
    }
    auto pase = s_pase_session.Get();
    if (!pase.HasValue() || Crypto::DRBG_get_bytes(s_nonce, sizeof(s_nonce)) != CHIP_NO_ERROR) {
        finish(MCTL_PAIR_FAILED, "the pairing session closed");
        return;
    }
    set_step(ST_CSR);
    OperationalCredentials::Commands::CSRRequest::Type req;
    req.CSRNonce = ByteSpan(s_nonce);
    invoke(pase.Value(), req, "CSRRequest failed", on_csr);
}

class PaseDelegate : public SessionEstablishmentDelegate
{
public:
    void OnSessionEstablishmentError(CHIP_ERROR err) override
    {
        /* A wrong code shows up here, as a failed PAKE exchange. */
        fail("the device did not accept the code", err);
    }

    void OnSessionEstablished(const SessionHandle & session) override
    {
        if (!pairing_live(current_gen())) {
            return;
        }
        s_pase_session.Grab(session);
        set_step(ST_ARM);
        GeneralCommissioning::Commands::ArmFailSafe::Type req;
        req.expiryLengthSeconds = kFailSafeS;
        req.breadcrumb          = 1;
        invoke(session, req, "ArmFailSafe failed", on_armed);
    }
};

PaseDelegate s_pase_delegate;

/* On the CHIP thread, once the browse has found the device. */
void pase_work(intptr_t gen)
{
    if (!pairing_live((uint32_t)gen)) {
        return;
    }
    /* Our fabric has to exist before a lamp can be told to trust it. Made here,
     * the first time, rather than at start: a box that never pairs a lamp never
     * takes a controller's fabric slot. */
    CHIP_ERROR err = mctl::fabric_ensure();
    if (err == CHIP_ERROR_BUSY) {
        finish(MCTL_PAIR_FAILED, "a controller is adding this box right now; try again in a minute");
        return;
    }
    if (err == CHIP_ERROR_NO_MEMORY) {
        finish(MCTL_PAIR_FAILED, "this box has no Matter fabric slot left; remove a controller first");
        return;
    }
    if (err != CHIP_NO_ERROR) {
        fail("could not make our fabric", err);
        return;
    }

    Inet::IPAddress addr;
    uint16_t        port;
    void           *netif;
    uint32_t        pin;
    portENTER_CRITICAL(&s_mux);
    memcpy(addr.Addr, s_p.addr, sizeof(addr.Addr));
    port  = s_p.port;
    netif = s_p.netif;
    pin   = s_p.pin;
    portEXIT_CRITICAL(&s_mux);

    Inet::InterfaceId iface(static_cast<struct netif *>(netif));
    const Transport::PeerAddress peer = Transport::PeerAddress::UDP(addr, port, addr.IsIPv6LinkLocal() ? iface
                                                                                           : Inet::InterfaceId::Null());
    char ip[Inet::IPAddress::kMaxStringLength];
    addr.ToString(ip);
    ESP_LOGI(TAG, "Found it at [%s]:%u; PASE", ip, port);

    set_step(ST_PASE);
    DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds32(kPairTimeoutS), timeout_cb, nullptr);

    SessionManager & sessions = Server::GetInstance().GetSecureSessionManager();
    auto session = sessions.CreateUnauthenticatedSession(peer, GetDefaultMRPConfig());
    if (!session.HasValue()) {
        fail("no session slot for the pairing", CHIP_ERROR_NO_MEMORY);
        return;
    }
    s_pase = Platform::New<PASESession>();
    if (!s_pase) {
        fail("no memory for the pairing", CHIP_ERROR_NO_MEMORY);
        return;
    }
    /* The three calls DeviceCommissioner::EstablishPASEConnection makes, on the
     * Server's objects. The exchange is made immediately before Pair, which takes
     * it over and frees it on failure. */
    Messaging::ExchangeContext * exch = Server::GetInstance().GetExchangeManager().NewContext(session.Value(), s_pase);
    if (!exch) {
        fail("no exchange for the pairing", CHIP_ERROR_NO_MEMORY);
        return;
    }
    err = s_pase->Pair(sessions, pin, GetLocalMRPConfig(), exch, &s_pase_delegate);
    if (err != CHIP_NO_ERROR) {
        fail("PASE did not start", err);
    }
}

/* ---------------------------------------------------------- the browse */

const char *txt_value(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) {
            return r->txt[i].value;
        }
    }
    return nullptr;
}

/* The discriminator the device advertises (TXT "D", the long one) against the
 * one in the code: a manual code carries only its top four bits. */
bool disc_matches(const mdns_result_t *r, bool short_disc, uint16_t disc)
{
    const char *d = txt_value(r, "D");
    if (!d || !d[0]) {
        return false;
    }
    const long v = strtol(d, nullptr, 10);
    if (v < 0 || v > 0xFFF) {
        return false;
    }
    return short_disc ? ((v >> 8) & 0xF) == disc : v == disc;
}

/* Rank of an address for PASE: a routable IPv6 first (a Thread device's comes
 * through the border router, and nothing else reaches it), then link-local IPv6
 * on the interface it was heard on, then IPv4. 0 unusable. */
int addr_rank(const esp_ip_addr_t & a)
{
    if (a.type == ESP_IPADDR_TYPE_V6) {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(a.u_addr.ip6.addr);
        const bool link_local = b[0] == 0xFE && (b[1] & 0xC0) == 0x80;
        return link_local ? 2 : 3;
    }
    if (a.type == ESP_IPADDR_TYPE_V4) {
        return a.u_addr.ip4.addr ? 1 : 0;
    }
    return 0;
}

void to_bytes(const esp_ip_addr_t & a, uint8_t out[16])
{
    if (a.type == ESP_IPADDR_TYPE_V6) {
        memcpy(out, a.u_addr.ip6.addr, 16);
        return;
    }
    /* IPv4-mapped, the form CHIP's IPAddress keeps a v4 address in. */
    uint32_t w[4] = { 0, 0, lwip_htonl(0xFFFF), a.u_addr.ip4.addr };
    memcpy(out, w, 16);
}

bool pick_address(const mdns_result_t *r, uint8_t out[16], void **netif)
{
    int best = 0;
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        const int rank = addr_rank(a->addr);
        if (rank > best) {
            best = rank;
            to_bytes(a->addr, out);
        }
    }
    if (best == 0 && r->hostname) {
        /* Answered without addresses: ask for the host. */
        esp_ip6_addr_t v6;
        esp_ip4_addr_t v4;
        if (mdns_query_aaaa(r->hostname, 1500, &v6) == ESP_OK) {
            esp_ip_addr_t a = {};
            a.type          = ESP_IPADDR_TYPE_V6;
            a.u_addr.ip6    = v6;
            best            = addr_rank(a);
            to_bytes(a, out);
        } else if (mdns_query_a(r->hostname, 1500, &v4) == ESP_OK) {
            esp_ip_addr_t a = {};
            a.type          = ESP_IPADDR_TYPE_V4;
            a.u_addr.ip4    = v4;
            best            = addr_rank(a);
            to_bytes(a, out);
        }
    }
    *netif = r->esp_netif ? esp_netif_get_netif_impl(r->esp_netif) : nullptr;
    if (!*netif) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        *netif           = sta ? esp_netif_get_netif_impl(sta) : nullptr;
    }
    return best > 0;
}

void find_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    bool     short_disc;
    uint16_t disc;
    portENTER_CRITICAL(&s_mux);
    short_disc = s_p.short_disc;
    disc       = s_p.disc;
    portEXIT_CRITICAL(&s_mux);

    bool found = false;
    for (int attempt = 0; attempt < kFindAttempts && !found && pairing_live(gen); attempt++) {
        mdns_result_t *results = nullptr;
        if (mdns_query_ptr("_matterc", "_udp", kFindWindowMs, 16, &results) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        for (const mdns_result_t *r = results; r && !found; r = r->next) {
            if (!disc_matches(r, short_disc, disc) || r->port == 0) {
                continue;
            }
            uint8_t addr[16];
            void   *netif = nullptr;
            if (!pick_address(r, addr, &netif)) {
                continue;
            }
            portENTER_CRITICAL(&s_mux);
            if (s_p.gen == gen) {
                memcpy(s_p.addr, addr, sizeof(addr));
                s_p.port  = r->port;
                s_p.netif = netif;
                s_p.found = true;
                found     = true;
            }
            portEXIT_CRITICAL(&s_mux);
            if (found) {
                ESP_LOGI(TAG, "Pairing: \"%s\" is in pairing mode", r->instance_name ? r->instance_name : "?");
            }
        }
        mdns_query_results_free(results);
    }
    if (found) {
        if (DeviceLayer::PlatformMgr().ScheduleWork(pase_work, (intptr_t)gen) != CHIP_NO_ERROR) {
            finish(MCTL_PAIR_FAILED, "the Matter task did not take the pairing");
        }
    } else if (pairing_live(gen)) {
        finish(MCTL_PAIR_FAILED, "no device in pairing mode with that code was found on the network");
    }
    vTaskDelete(nullptr);
}

/* -------------------------------------------------- endpoint discovery */

/* Endpoints a device may have in all, lamps or not - a Hue Bridge lists its
 * sensors and switches too - and lamps kept per device while reading. */
constexpr int kMaxEps  = 64;
constexpr int kMaxLamps = CONFIG_ZBC_MAX_MATTER_LIGHTS;

void copy_span(char *out, size_t cap, const CharSpan & in)
{
    size_t n = in.size() < cap - 1 ? in.size() : cap - 1;
    /* A cut inside a UTF-8 character would leave half of it behind, so a cut
     * backs off to the start of the character it falls in - as mctl_disc_copy
     * does. A name that fits is copied whole: "Café" keeps its é. */
    if (n < in.size()) {
        while (n > 0 && ((unsigned char)in.data()[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(out, in.data(), n);
    out[n] = '\0';
}

class Discoverer : public ReadClient::Callback
{
public:
    NodeId                 node        = kUndefinedNodeId;
    uint8_t                lamp_fabric = 0;
    mctl::discover_done_fn done        = nullptr;
    bool                   busy        = false;
    /* The last read found lamps and the table had no room for one of them. */
    bool                   full        = false;

    CHIP_ERROR Start(NodeId n, uint8_t fab, const SessionHandle & session, mctl::discover_done_fn cb)
    {
        VerifyOrReturnError(!busy, CHIP_ERROR_BUSY);
        node        = n;
        lamp_fabric = fab;
        done        = cb;
        phase       = 1;
        n_types     = 0;
        n_lamps     = 0;
        full        = false;
        error       = CHIP_NO_ERROR;
        memset(&basic, 0, sizeof(basic));
        session_.Grab(session);
        busy = true;
        CHIP_ERROR err = Read();
        if (err != CHIP_NO_ERROR) {
            busy = false;
            session_.Release();
        }
        return err;
    }

    void OnAttributeData(const ConcreteDataAttributePath & path, TLV::TLVReader * data, const StatusIB & status) override
    {
        if (!data || !status.IsSuccess()) {
            return;
        }
        if (phase == 1) {
            NoteTypes(path, *data);
        } else {
            NoteDetail(path, *data);
        }
    }

    void OnError(CHIP_ERROR err) override { error = err; }

    void OnDone(ReadClient * client) override
    {
        Platform::Delete(client);
        if (error == CHIP_NO_ERROR && phase == 1) {
            if (CountLights() == 0) {
                Finish(CHIP_NO_ERROR);
                return;
            }
            phase = 2;
            CHIP_ERROR err = Read();
            if (err != CHIP_NO_ERROR) {
                Finish(err);
            }
            return;
        }
        Finish(error);
    }

private:
    int              phase   = 0;
    CHIP_ERROR       error   = CHIP_NO_ERROR;
    SessionHolder    session_;
    /* Phase 1: every endpoint's device types, reduced to two flags. */
    struct { uint16_t ep; bool light, bridged; } types[kMaxEps];
    int              n_types = 0;
    /* Phase 2: the light endpoints in detail. */
    mctl_disc_ep_t   lamps[kMaxLamps];
    int              n_lamps = 0;
    mctl_disc_node_t basic;
    AttributePathParams paths[6];

    CHIP_ERROR Read()
    {
        auto session = session_.Get();
        VerifyOrReturnError(session.HasValue(), CHIP_ERROR_INCORRECT_STATE);
        size_t n = 0;
        if (phase == 1) {
            paths[n++] = AttributePathParams(Descriptor::Id, Descriptor::Attributes::DeviceTypeList::Id);
        } else {
            paths[n++] = AttributePathParams(Descriptor::Id, Descriptor::Attributes::ServerList::Id);
            paths[n++] = AttributePathParams(ColorControl::Id, ColorControl::Attributes::FeatureMap::Id);
            paths[n++] = AttributePathParams(ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMinMireds::Id);
            paths[n++] = AttributePathParams(ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id);
            /* Two whole clusters rather than ten attributes: a device is only
             * promised nine paths per read, and these are small. */
            paths[n++] = AttributePathParams(kRootEndpointId, BasicInformation::Id);
            paths[n++] = AttributePathParams(BridgedDeviceBasicInformation::Id, kInvalidAttributeId);
        }
        ReadPrepareParams params(session.Value());
        params.mpAttributePathParamsList    = paths;
        params.mAttributePathParamsListSize = n;
        params.mIsFabricFiltered            = false;
        auto client = Platform::MakeUnique<ReadClient>(InteractionModelEngine::GetInstance(),
                                                       &Server::GetInstance().GetExchangeManager(), *this,
                                                       ReadClient::InteractionType::Read);
        VerifyOrReturnError(client, CHIP_ERROR_NO_MEMORY);
        ReturnErrorOnFailure(client->SendRequest(params));
        client.release();
        return CHIP_NO_ERROR;
    }

    void NoteType(EndpointId ep, uint32_t type)
    {
        for (int i = 0; i < n_types; i++) {
            if (types[i].ep == ep) {
                types[i].light |= mctl_disc_light_type(type);
                types[i].bridged |= type == MCTL_DT_BRIDGED_NODE;
                return;
            }
        }
        if (n_types < kMaxEps) {
            types[n_types].ep      = ep;
            types[n_types].light   = mctl_disc_light_type(type);
            types[n_types].bridged = type == MCTL_DT_BRIDGED_NODE;
            n_types++;
        }
    }

    void NoteTypes(const ConcreteDataAttributePath & path, TLV::TLVReader & data)
    {
        if (path.mClusterId != Descriptor::Id || path.mAttributeId != Descriptor::Attributes::DeviceTypeList::Id) {
            return;
        }
        /* A long list can arrive in pieces: the list, then items appended. */
        if (path.IsListItemOperation()) {
            Descriptor::Structs::DeviceTypeStruct::DecodableType item;
            if (DataModel::Decode(data, item) == CHIP_NO_ERROR) {
                NoteType(path.mEndpointId, item.deviceType);
            }
            return;
        }
        Descriptor::Attributes::DeviceTypeList::TypeInfo::DecodableType list;
        if (DataModel::Decode(data, list) != CHIP_NO_ERROR) {
            return;
        }
        auto it = list.begin();
        while (it.Next()) {
            NoteType(path.mEndpointId, it.GetValue().deviceType);
        }
    }

    int CountLights()
    {
        n_lamps = 0;
        for (int i = 0; i < n_types && n_lamps < kMaxLamps; i++) {
            if (types[i].light) {
                memset(&lamps[n_lamps], 0, sizeof(lamps[n_lamps]));
                lamps[n_lamps].ep      = types[i].ep;
                lamps[n_lamps].light   = true;
                lamps[n_lamps].bridged = types[i].bridged;
                n_lamps++;
            }
        }
        return n_lamps;
    }

    mctl_disc_ep_t *Lamp(EndpointId ep)
    {
        for (int i = 0; i < n_lamps; i++) {
            if (lamps[i].ep == ep) {
                return &lamps[i];
            }
        }
        return nullptr;
    }

    void NoteCluster(mctl_disc_ep_t *e, ClusterId c)
    {
        e->onoff |= c == OnOff::Id;
        e->level |= c == LevelControl::Id;
        e->color |= c == ColorControl::Id;
    }

    template <typename T>
    static bool Decode(TLV::TLVReader & data, T & out)
    {
        return DataModel::Decode(data, out) == CHIP_NO_ERROR;
    }

    void NoteDetail(const ConcreteDataAttributePath & path, TLV::TLVReader & data)
    {
        if (path.mClusterId == BasicInformation::Id && path.mEndpointId == kRootEndpointId) {
            CharSpan s;
            switch (path.mAttributeId) {
            case BasicInformation::Attributes::VendorName::Id:
                if (Decode(data, s)) copy_span(basic.vendor, sizeof(basic.vendor), s);
                break;
            case BasicInformation::Attributes::ProductName::Id:
                if (Decode(data, s)) copy_span(basic.product, sizeof(basic.product), s);
                break;
            case BasicInformation::Attributes::NodeLabel::Id:
                if (Decode(data, s)) copy_span(basic.label, sizeof(basic.label), s);
                break;
            case BasicInformation::Attributes::VendorID::Id: {
                VendorId v;
                if (Decode(data, v)) basic.vendor_id = static_cast<uint16_t>(v);
                break;
            }
            case BasicInformation::Attributes::ProductID::Id:
                Decode(data, basic.product_id);
                break;
            default:
                break;
            }
            return;
        }
        mctl_disc_ep_t *e = Lamp(path.mEndpointId);
        if (!e) {
            return;
        }
        if (path.mClusterId == Descriptor::Id && path.mAttributeId == Descriptor::Attributes::ServerList::Id) {
            if (path.IsListItemOperation()) {
                ClusterId c;
                if (Decode(data, c)) NoteCluster(e, c);
                return;
            }
            Descriptor::Attributes::ServerList::TypeInfo::DecodableType list;
            if (Decode(data, list)) {
                auto it = list.begin();
                while (it.Next()) {
                    NoteCluster(e, it.GetValue());
                }
            }
            return;
        }
        if (path.mClusterId == ColorControl::Id) {
            switch (path.mAttributeId) {
            case ColorControl::Attributes::FeatureMap::Id:
                Decode(data, e->cc_features);
                break;
            case ColorControl::Attributes::ColorTempPhysicalMinMireds::Id:
                Decode(data, e->ct_min);
                break;
            case ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id:
                Decode(data, e->ct_max);
                break;
            default:
                break;
            }
            return;
        }
        if (path.mClusterId == BridgedDeviceBasicInformation::Id) {
            CharSpan s;
            switch (path.mAttributeId) {
            case BridgedDeviceBasicInformation::Attributes::NodeLabel::Id:
                if (Decode(data, s)) copy_span(e->label, sizeof(e->label), s);
                break;
            case BridgedDeviceBasicInformation::Attributes::VendorName::Id:
                if (Decode(data, s)) copy_span(e->vendor, sizeof(e->vendor), s);
                break;
            case BridgedDeviceBasicInformation::Attributes::ProductName::Id:
                if (Decode(data, s)) copy_span(e->product, sizeof(e->product), s);
                break;
            case BridgedDeviceBasicInformation::Attributes::VendorID::Id: {
                VendorId v;
                if (Decode(data, v)) e->vendor_id = static_cast<uint16_t>(v);
                break;
            }
            case BridgedDeviceBasicInformation::Attributes::ProductID::Id:
                Decode(data, e->product_id);
                break;
            default:
                break;
            }
        }
    }

    int Store()
    {
        int lamps_on_node = 0;
        for (int i = 0; i < n_lamps; i++) {
            lamps_on_node += mctl_disc_is_lamp(&lamps[i]);
        }
        int stored = 0;
        for (int i = 0; i < n_lamps; i++) {
            const mctl_disc_ep_t *e = &lamps[i];
            if (!mctl_disc_is_lamp(e)) {
                continue;
            }
            zbc_mlight_t row = {};
            row.node_id       = node;
            row.remote_ep     = e->ep;
            row.lamp_fabric   = lamp_fabric;
            row.features      = mctl_disc_features(e);
            row.flags         = e->bridged ? ZBC_MLIGHT_BRIDGED : 0;
            if (row.features & ZBC_FEAT_COLOR_TEMP) {
                row.ct_min_mireds = e->ct_min;
                row.ct_max_mireds = e->ct_max;
            }
            mctl_disc_name(e, &basic, lamps_on_node, row.name, sizeof(row.name));
            const char *vendor, *product;
            mctl_disc_identity(e, &basic, &vendor, &product, &row.vendor_id, &row.product_id);
            strlcpy(row.vendor, vendor, sizeof(row.vendor));
            strlcpy(row.product, product, sizeof(row.product));
            uint16_t        id  = 0;
            const esp_err_t put = mlight_db_put(&row, &id);
            if (put == ESP_OK) {
                stored++;
                ESP_LOGI(TAG, "Lamp %u: \"%s\" (%s %s), endpoint %u, features 0x%02x, %u-%u mireds%s", id, row.name,
                         row.vendor, row.product, e->ep, row.features, row.ct_min_mireds, row.ct_max_mireds,
                         e->bridged ? ", behind a hub" : "");
            } else {
                full = full || put == ESP_ERR_NO_MEM;
                ESP_LOGW(TAG, "No room for lamp \"%s\"", row.name);
            }
        }
        return stored;
    }

    void Finish(CHIP_ERROR err)
    {
        const int stored = err == CHIP_NO_ERROR ? Store() : 0;
        busy = false;
        session_.Release();
        mctl::discover_done_fn cb = done;
        done = nullptr;
        if (cb) {
            cb(node, err, stored);
        }
    }
};

Discoverer *s_disc;

void on_discovered(NodeId node, CHIP_ERROR err, int lamps)
{
    if (err != CHIP_NO_ERROR) {
        fail("could not read the device's endpoints", err);
        return;
    }
    if (lamps == 0) {
        /* Paired, and of no use here: tell it to forget us again rather than
         * leave a fabric behind in its table. Said as what it is - a device whose
         * lamps found no room is not a device without lamps. */
        mctl_remove_node(node, s_lamp_fabric);
        finish(MCTL_PAIR_FAILED, s_disc && s_disc->full ? "this box has no room for another Matter lamp"
                                                        : "that device has no light on it");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_p.lamps = (uint8_t)lamps;
    portEXIT_CRITICAL(&s_mux);
    mctl::ctl_reload_now();
    finish(MCTL_PAIR_DONE, nullptr);
}

} // namespace

namespace mctl {

CHIP_ERROR discover_start(NodeId node, uint8_t lamp_fabric, const SessionHandle & session, discover_done_fn done)
{
    if (!s_disc) {
        /* About 2.5 KB: allocated for the first pairing or rescan, then kept,
         * since a box that pairs once usually pairs again. */
        s_disc = Platform::New<Discoverer>();
        VerifyOrReturnError(s_disc, CHIP_ERROR_NO_MEMORY);
    }
    return s_disc->Start(node, lamp_fabric, session, done);
}

bool commission_busy()
{
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_p.state == MCTL_PAIR_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

esp_err_t commission_start(uint32_t pin, bool short_disc, uint16_t disc)
{
    uint32_t gen;
    portENTER_CRITICAL(&s_mux);
    if (s_p.state == MCTL_PAIR_RUNNING) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_p.gen++;
    gen            = s_p.gen;
    s_p.state      = MCTL_PAIR_RUNNING;
    s_p.step       = ST_FIND;
    s_p.error[0]   = '\0';
    s_p.node       = 0;
    s_p.lamps      = 0;
    s_p.started_us = esp_timer_get_time();
    memset(s_p.heap_low, 0, sizeof(s_p.heap_low));
    s_p.pin        = pin;
    s_p.short_disc = short_disc;
    s_p.disc       = disc;
    s_p.found      = false;
    portEXIT_CRITICAL(&s_mux);

    heap_watch_mark("mpair_start", disc);
    ESP_LOGI(TAG, "Pairing: looking for a device with %s discriminator %u", short_disc ? "short" : "long", disc);
    if (xTaskCreate(find_task, "mpair_find", 4096, (void *)(uintptr_t)gen, 3, nullptr) != pdPASS) {
        finish(MCTL_PAIR_FAILED, "no memory to start looking");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void commission_status(mctl_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    out->pair_state = s_p.state;
    strlcpy(out->pair_step, STEP_NAMES[s_p.step < sizeof(STEP_NAMES) / sizeof(STEP_NAMES[0]) ? s_p.step : 0],
            sizeof(out->pair_step));
    strlcpy(out->pair_error, s_p.error, sizeof(out->pair_error));
    out->pair_node_id = s_p.node;
    out->pair_lamps   = s_p.lamps;
    out->pair_ago_s   = s_p.started_us ? (uint32_t)((esp_timer_get_time() - s_p.started_us) / 1000000) : 0;
    memcpy(out->pair_heap_low, s_p.heap_low, sizeof(out->pair_heap_low));
    for (int i = 0; i < MCTL_PAIR_STEPS; i++) {
        out->pair_step_names[i] = STEP_NAMES[i];
    }
    portEXIT_CRITICAL(&s_mux);
}

} // namespace mctl

/* Parses the code on the caller's task - the parsers are plain functions - and
 * hands the rest to the browse. */
extern "C" esp_err_t mctl_pair(const char *code)
{
    if (!mctl_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!code) {
        return ESP_ERR_INVALID_ARG;
    }
    std::string s;
    for (const char *c = code; *c; c++) {
        if (*c != ' ' && *c != '\t') {
            s += *c;
        }
    }
    SetupPayload payload;
    CHIP_ERROR   err;
    if (s.rfind("MT:", 0) == 0) {
        err = QRCodeSetupPayloadParser(s).populatePayload(payload);
    } else {
        err = ManualSetupPayloadParser(s).populatePayload(payload);
    }
    if (err != CHIP_NO_ERROR || !PayloadContents::IsValidSetupPIN(payload.setUpPINCode)) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool     short_disc = payload.discriminator.IsShortDiscriminator();
    const uint16_t disc       = short_disc ? payload.discriminator.GetShortValue() : payload.discriminator.GetLongValue();
    if (mctl::commission_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Our fabric before anything else, so what stops it - a full fabric table, a
     * controller adding the box right now - is answered to the request rather
     * than as a pairing that fails later. Under the CHIP lock: the FabricTable is
     * the Server's. A key, a certificate and a signature: well under a second. */
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        err = mctl::fabric_ensure();
    }
    if (err == CHIP_ERROR_BUSY) {
        return MCTL_ERR_BRIDGE_BUSY;
    }
    if (err == CHIP_ERROR_NO_MEMORY) {
        return MCTL_ERR_NO_FABRIC_SLOT;
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Could not make our fabric: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }
    return mctl::commission_start(payload.setUpPINCode, short_disc, disc);
}
