/* Our own fabric, in the Matter server's FabricTable.
 *
 * Why not a controller stack of its own. esp_matter's commissioner is compiled
 * out whenever its server is on (ESP_MATTER_COMMISSIONER_ENABLE depends on
 * !ESP_MATTER_ENABLE_MATTER_SERVER). CHIP's DeviceControllerFactory would bring a
 * second session manager and exchange stack, and it re-Inits the
 * InteractionModelEngine singleton, which overwrites the server's exchange
 * manager and report scheduler - the bridge's subscriptions to Apple Home stop.
 * So the controller runs on the Server's stack: one more fabric beside the
 * controllers' own, PASE and CASE on the Server's SessionManager, reads,
 * subscriptions and invokes through its InteractionModelEngine. That is the shape
 * esp_matter's own server-mode cluster commands take.
 *
 * The fabric is made once, on the first pairing, and advertised like any
 * other: we open every session ourselves, but a lamp that restarts resumes its
 * subscriptions by setting up CASE to the subscriber, which needs to find us -
 * and CHIP does not keep "not advertised" across a restart anyway. It holds no
 * ACL entries, so no lamp can reach the bridge's data model through it.
 *
 * The root key and the IPK are kept in CHIP's KVS, in nvs_matter, beside the
 * fabric they belong to: a Matter reset forgets all three together, and the lamp
 * rows left behind show as needing pairing again. A fabric entry lost on its own
 * is rebuilt from the same root, fabric id and node id, which every lamp still
 * trusts.
 *
 * Certificates do not depend on the clock: they are valid from a fixed date in
 * the past and never expire, so nothing waits on SNTP. */
#include <string.h>

#include "esp_log.h"

#include <app/server/Server.h>
#include <credentials/CHIPCert.h>
#include <credentials/GroupDataProvider.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/asn1/ASN1.h>
#include <lib/core/CHIPVendorIdentifiers.hpp>
#include <lib/support/ScopedMemoryBuffer.h>

#include "matter_priv.h"

static const char *TAG = "mfabric";

using namespace chip;
using namespace chip::Credentials;

namespace {

/* One KVS entry. Version it like a blob: a change of layout is a new key or a
 * migration, never a silent misread of a root key. */
constexpr char    kKey[]     = "zbc/mctl/fabric";
constexpr uint8_t kVersion   = 1;
constexpr char    kLabel[]   = "Elixcon Light Hub lamps";

struct stored_t {
    uint8_t  version;
    uint8_t  key_len;
    uint8_t  reserved[6];
    uint64_t fabric_id;
    uint64_t node_id;           /* ours */
    uint64_t rcac_id;           /* the MatterRCACId in our root's subject */
    uint8_t  ipk[Crypto::CHIP_CRYPTO_SYMMETRIC_KEY_LENGTH_BYTES];
    uint8_t  key[Crypto::kP256_PublicKey_Length + Crypto::kP256_PrivateKey_Length];
};

stored_t          s_st;
bool              s_have_st;
FabricIndex       s_index = kUndefinedFabricIndex;

/* 2021-01-01, as CHIP's example issuer uses: before any lamp's Last Known Good
 * Time, so a lamp with no clock of its own still accepts the chain. */
uint32_t validity_start()
{
    ASN1::ASN1UniversalTime t = {};
    t.Year  = 2021;
    t.Month = 1;
    t.Day   = 1;
    uint32_t epoch = 0;
    return ASN1ToChipEpochTime(t, epoch) == CHIP_NO_ERROR ? epoch : 0;
}

CHIP_ERROR random_u64(uint64_t & out)
{
    return Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&out), sizeof(out));
}

CHIP_ERROR load_key(Crypto::P256Keypair & key)
{
    Crypto::P256SerializedKeypair ser;
    VerifyOrReturnError(s_st.key_len <= ser.Capacity() && s_st.key_len == sizeof(s_st.key), CHIP_ERROR_INTERNAL);
    memcpy(ser.Bytes(), s_st.key, s_st.key_len);
    ReturnErrorOnFailure(ser.SetLength(s_st.key_len));
    return key.Deserialize(ser);
}

CHIP_ERROR read_stored()
{
    uint16_t len = sizeof(s_st);
    CHIP_ERROR err = Server::GetInstance().GetPersistentStorage().SyncGetKeyValue(kKey, &s_st, len);
    if (err != CHIP_NO_ERROR) {
        return err;
    }
    if (len != sizeof(s_st) || s_st.version != kVersion) {
        ESP_LOGW(TAG, "Stored fabric key is from another build, ignoring it");
        return CHIP_ERROR_INCORRECT_STATE;
    }
    return CHIP_NO_ERROR;
}

/* A root, a fabric id, our node id and an IPK, made once and never changed: every
 * lamp paired afterwards trusts exactly these. */
CHIP_ERROR make_stored()
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.version = kVersion;
    do {
        ReturnErrorOnFailure(random_u64(s_st.fabric_id));
    } while (s_st.fabric_id == kUndefinedFabricId);
    s_st.node_id = mctl::fabric_new_node_id();
    VerifyOrReturnError(s_st.node_id != kUndefinedNodeId, CHIP_ERROR_INTERNAL);
    ReturnErrorOnFailure(random_u64(s_st.rcac_id));
    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(s_st.ipk, sizeof(s_st.ipk)));

    Platform::UniquePtr<Crypto::P256Keypair> key(Platform::New<Crypto::P256Keypair>());
    VerifyOrReturnError(key, CHIP_ERROR_NO_MEMORY);
    ReturnErrorOnFailure(key->Initialize(Crypto::ECPKeyTarget::ECDSA));
    Crypto::P256SerializedKeypair ser;
    ReturnErrorOnFailure(key->Serialize(ser));
    VerifyOrReturnError(ser.Length() == sizeof(s_st.key), CHIP_ERROR_INTERNAL);
    memcpy(s_st.key, ser.Bytes(), ser.Length());
    s_st.key_len = (uint8_t)ser.Length();

    return Server::GetInstance().GetPersistentStorage().SyncSetKeyValue(kKey, &s_st, sizeof(s_st));
}

/* Cannot fail: one attribute in an empty DN. */
ChipDN root_dn()
{
    ChipDN dn;
    (void)dn.AddAttribute_MatterRCACId(s_st.rcac_id);
    return dn;
}

/* X.509 first, because that is what CHIP generates, then the Matter TLV encoding
 * everything on the wire and in the FabricTable uses. Both buffers on the heap:
 * the CHIP task's stack is 8 KB and the signing below it is deep. */
CHIP_ERROR to_chip_cert(const ByteSpan & der, MutableByteSpan & out)
{
    return ConvertX509CertToChipCert(der, out);
}

CHIP_ERROR make_root_cert(const Crypto::P256Keypair & key, MutableByteSpan & out)
{
    Platform::ScopedMemoryBuffer<uint8_t> der;
    VerifyOrReturnError(der.Alloc(kMaxDERCertLength), CHIP_ERROR_NO_MEMORY);
    MutableByteSpan derSpan(der.Get(), kMaxDERCertLength);
    const ChipDN dn = root_dn();
    X509CertRequestParams req = { 1, validity_start(), kNullCertTime, dn, dn };
    ReturnErrorOnFailure(NewRootX509Cert(req, key, derSpan));
    return to_chip_cert(derSpan, out);
}

CHIP_ERROR make_noc(const Crypto::P256Keypair & root, const Crypto::P256PublicKey & pub, NodeId node,
                    MutableByteSpan & out)
{
    ChipDN subject;
    ReturnErrorOnFailure(subject.AddAttribute_MatterFabricId(s_st.fabric_id));
    ReturnErrorOnFailure(subject.AddAttribute_MatterNodeId(node));
    uint64_t serial = 0;
    ReturnErrorOnFailure(random_u64(serial));
    serial &= 0x7FFFFFFFFFFFFFFFull;   /* a positive INTEGER */
    Platform::ScopedMemoryBuffer<uint8_t> der;
    VerifyOrReturnError(der.Alloc(kMaxDERCertLength), CHIP_ERROR_NO_MEMORY);
    MutableByteSpan derSpan(der.Get(), kMaxDERCertLength);
    X509CertRequestParams req = { static_cast<int64_t>(serial ? serial : 1), validity_start(), kNullCertTime, subject,
                                  root_dn() };
    ReturnErrorOnFailure(NewNodeOperationalX509Cert(req, pub, root, derSpan));
    return to_chip_cert(derSpan, out);
}

FabricIndex find_ours()
{
    Platform::UniquePtr<Crypto::P256Keypair> key(Platform::New<Crypto::P256Keypair>());
    if (!key || load_key(*key) != CHIP_NO_ERROR) {
        return kUndefinedFabricIndex;
    }
    FabricTable & table = Server::GetInstance().GetFabricTable();
    for (const FabricInfo & fi : table) {
        Crypto::P256PublicKey pub;
        if (fi.GetFabricId() == s_st.fabric_id && table.FetchRootPubkey(fi.GetFabricIndex(), pub) == CHIP_NO_ERROR &&
            pub.Matches(key->Pubkey())) {
            return fi.GetFabricIndex();
        }
    }
    return kUndefinedFabricIndex;
}

CHIP_ERROR set_ipk(FabricIndex index)
{
    const FabricInfo * fi = Server::GetInstance().GetFabricTable().FindFabricWithIndex(index);
    VerifyOrReturnError(fi, CHIP_ERROR_INTERNAL);
    uint8_t         cfid[sizeof(uint64_t)];
    MutableByteSpan cfidSpan(cfid);
    ReturnErrorOnFailure(fi->GetCompressedFabricIdBytes(cfidSpan));
    return SetSingleIpkEpochKey(Server::GetInstance().GetGroupDataProvider(), index, ByteSpan(s_st.ipk), cfidSpan);
}

} // namespace

namespace mctl {

NodeId fabric_new_node_id()
{
    for (int i = 0; i < 16; i++) {
        uint64_t id = 0;
        if (random_u64(id) == CHIP_NO_ERROR && IsOperationalNodeId(id)) {
            return id;
        }
    }
    return kUndefinedNodeId;
}

void fabric_load()
{
    s_have_st = read_stored() == CHIP_NO_ERROR;
    s_index   = s_have_st ? find_ours() : kUndefinedFabricIndex;
    if (s_index != kUndefinedFabricIndex) {
        ESP_LOGI(TAG, "Our fabric is index %u, node 0x%016llx", s_index, (unsigned long long)s_st.node_id);
    } else if (s_have_st) {
        ESP_LOGW(TAG, "Our root key is stored but its fabric is gone; it is made again at the next pairing");
    }
}

/* The last attempt's failure, for /api/matter: making the fabric is the one
 * step of pairing whose reason would otherwise be on the serial port only. */
static char s_error[64];

static CHIP_ERROR failed(const char *step, CHIP_ERROR err)
{
    snprintf(s_error, sizeof(s_error), "%s: %" CHIP_ERROR_FORMAT, step, err.Format());
    ESP_LOGE(TAG, "Could not make our fabric - %s", s_error);
    return err;
}

CHIP_ERROR fabric_ensure()
{
    if (s_index != kUndefinedFabricIndex) {
        return CHIP_NO_ERROR;
    }
    Server & server = Server::GetInstance();
    /* The FabricTable's pending state is the one the Operational Credentials
     * server fills while a controller commissions the bridge; ours would land in
     * the middle of theirs. */
    if (server.GetFailSafeContext().IsFailSafeArmed()) {
        return failed("a controller is commissioning the bridge", CHIP_ERROR_BUSY);
    }
    FabricTable & table = server.GetFabricTable();
    if (table.FabricCount() >= CHIP_CONFIG_MAX_FABRICS) {
        return failed("no fabric slot", CHIP_ERROR_NO_MEMORY);
    }

    /* The key first, and saved: if the fabric step fails the next attempt uses
     * the same one, and if saving it failed no fabric is made that nobody could
     * sign for again. */
    CHIP_ERROR err;
    if (!s_have_st) {
        if ((err = make_stored()) != CHIP_NO_ERROR) {
            return failed("root key", err);
        }
        s_have_st = true;
    }

    Platform::UniquePtr<Crypto::P256Keypair> root(Platform::New<Crypto::P256Keypair>());
    Platform::ScopedMemoryBuffer<uint8_t>    rcac, noc, csr;
    if (!root || !rcac.Alloc(kMaxCHIPCertLength) || !noc.Alloc(kMaxCHIPCertLength) ||
        !csr.Alloc(Crypto::kMIN_CSR_Buffer_Size)) {
        return failed("memory", CHIP_ERROR_NO_MEMORY);
    }
    MutableByteSpan rcacSpan(rcac.Get(), kMaxCHIPCertLength);
    MutableByteSpan nocSpan(noc.Get(), kMaxCHIPCertLength);
    MutableByteSpan csrSpan(csr.Get(), Crypto::kMIN_CSR_Buffer_Size);
    if ((err = load_key(*root)) != CHIP_NO_ERROR) {
        return failed("loading the root key", err);
    }
    if ((err = make_root_cert(*root, rcacSpan)) != CHIP_NO_ERROR) {
        return failed("root certificate", err);
    }

    /* One locked block from here to the commit, and any failure reverts: the
     * pending state must never be left half-filled for the next controller.
     * The order is a commissionee's own - CSRRequest, AddTrustedRootCertificate,
     * AddNOC: the table allocates a pending key only while nothing else is
     * pending. */
    FabricIndex           index = kUndefinedFabricIndex;
    Crypto::P256PublicKey pub;
    const char           *step  = "operational key";
    err = table.AllocatePendingOperationalKey(NullOptional, csrSpan);
    if (err == CHIP_NO_ERROR) {
        step = "trusted root";
        err  = table.AddNewPendingTrustedRootCert(rcacSpan);
    }
    if (err == CHIP_NO_ERROR) {
        step = "own CSR";
        err  = Crypto::VerifyCertificateSigningRequest(csrSpan.data(), csrSpan.size(), pub);
    }
    if (err == CHIP_NO_ERROR) {
        step = "own certificate";
        err  = make_noc(*root, pub, s_st.node_id, nocSpan);
    }
    if (err == CHIP_NO_ERROR) {
        step = "adding the fabric";
        err  = table.AddNewPendingFabricWithOperationalKeystore(nocSpan, ByteSpan(), fabric_vendor_id(), &index,
                                                                FabricTable::AdvertiseIdentity::Yes);
    }
    if (err == CHIP_NO_ERROR) {
        step = "committing the fabric";
        err  = table.CommitPendingFabricData();
    }
    if (err != CHIP_NO_ERROR) {
        table.RevertPendingFabricData();
        return failed(step, err);
    }
    /* Only what /api/matter shows; a fabric without it works the same. */
    (void)table.SetFabricLabel(index, CharSpan::fromCharString(kLabel));
    err = set_ipk(index);
    if (err != CHIP_NO_ERROR) {
        /* Without it no CASE session can be set up; a fabric that cannot be used
         * is not worth a slot. */
        (void)table.Delete(index);
        return failed("identity protection key", err);
    }
    s_index    = index;
    s_error[0] = '\0';
    ESP_LOGI(TAG, "Made our fabric: index %u, fabric 0x%016llx, node 0x%016llx", index,
             (unsigned long long)s_st.fabric_id, (unsigned long long)s_st.node_id);
    return CHIP_NO_ERROR;
}

const char *fabric_error()
{
    return s_error;
}

FabricIndex fabric_index()
{
    return s_index;
}

NodeId fabric_own_node()
{
    return s_index != kUndefinedFabricIndex ? s_st.node_id : kUndefinedNodeId;
}

uint16_t fabric_vendor_id()
{
    /* The same test vendor the bridge's own attestation carries. */
    return static_cast<uint16_t>(VendorId::TestVendor1);
}

CHIP_ERROR fabric_root_cert(MutableByteSpan & out)
{
    VerifyOrReturnError(s_index != kUndefinedFabricIndex, CHIP_ERROR_INCORRECT_STATE);
    return Server::GetInstance().GetFabricTable().FetchRootCert(s_index, out);
}

CHIP_ERROR fabric_issue_noc(const Crypto::P256PublicKey & pub, NodeId node, MutableByteSpan & out)
{
    VerifyOrReturnError(s_index != kUndefinedFabricIndex, CHIP_ERROR_INCORRECT_STATE);
    Platform::UniquePtr<Crypto::P256Keypair> root(Platform::New<Crypto::P256Keypair>());
    VerifyOrReturnError(root, CHIP_ERROR_NO_MEMORY);
    ReturnErrorOnFailure(load_key(*root));
    return make_noc(*root, pub, node, out);
}

ByteSpan fabric_ipk()
{
    return ByteSpan(s_st.ipk);
}

} // namespace mctl
