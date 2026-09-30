// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Reference Stratum V2 CPU miner for ApertureCoin.
//
// Connects to a Template Provider (sv2-tp) over the encrypted SV2 transport
// (Noise NX), subscribes to the Template Distribution Protocol, builds its own
// coinbase (payout + the template's mandatory outputs, e.g. the development
// fund), grinds ApertureMatMul nonces on all threads and submits solutions.
// The miner chooses its own coinbase; the transaction set comes from the
// miner's own node through its own TP. That is the decentralization property
// Stratum V2 provides.
//
// ApertureMatMul v2 (doc/pouw-v2.md): for v2 templates the Template Provider
// also sends the block's embedding requests (extension 0x4150). The miner runs
// the protocol model's forward pass over them itself, searches PoW tickets over
// the resulting activations with the pinned weights, and submits the ticket
// with SubmitUsefulWorkSolution.

#include <arith_uint256.h>
#include <base58.h>
#include <crypto/matmulpow.h>
#include <crypto/matmulpow_v2.h>
#include <crypto/matmulpow_v2_kernel.h>
#include <model/apm.h>
#include <model/intmodel.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <random.h>
#include <script/script.h>
#include <streams.h>
#include <sv2/messages.h>
#include <sv2/transport.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    std::string port{"8442"};
    std::string authority;
    CScript payout{CScript() << OP_TRUE};
    unsigned int dim{512};
    unsigned int threads{std::max(1u, std::thread::hardware_concurrency())};
    int max_blocks{-1};
    std::string model_path;   //!< -protocolmodel=<file.apm>
    bool tiny_model{false};   //!< -tinymodel: built-in regtest model
};

/** Useful work of a v2 template, with the miner's own forward-pass trace. */
struct UsefulWork {
    std::vector<unsigned char> batch_root, model_id;
    unsigned int rank{0};
    std::vector<std::vector<uint32_t>> requests;
    std::vector<intmodel::OpTrace> trace;
};

struct Template {
    uint64_t id{0};
    bool future{false};
    uint32_t version{0};
    uint32_t coinbase_version{0};
    std::vector<unsigned char> coinbase_prefix;
    uint32_t coinbase_sequence{0};
    uint64_t value_remaining{0};
    std::vector<CTxOut> outputs;
    uint32_t locktime{0};
    std::vector<uint256> merkle_path;
    std::shared_ptr<const UsefulWork> useful; //!< set for v2 templates once announced
};

constexpr uint32_t VERSION_POWV2{1 << 8};
std::unique_ptr<intmodel::IntModel> g_model;
matmulpow_v2::Backend g_backend{matmulpow_v2::BestBackend()};

struct Job {
    uint64_t generation{0};
    Template tmpl;
    uint256 prev_hash;
    uint32_t ntime{0};
    uint32_t nbits{0};
    arith_uint256 target;
};

std::mutex g_job_mutex;
std::optional<Job> g_job;
std::atomic<uint64_t> g_generation{0};
std::atomic<int> g_found{0};
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_hashes{0};
//! Generation of the last job a solution was submitted for (one per job).
std::atomic<uint64_t> g_solved_generation{0};

int Connect(const Options& opt)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res{nullptr};
    if (getaddrinfo(opt.host.c_str(), opt.port.c_str(), &hints, &res) != 0) return -1;
    int fd{-1};
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

class Connection
{
public:
    Connection(int fd, XOnlyPubKey authority) : m_fd(fd), m_transport(GenerateRandomKey(), authority) {}

    //! Flush everything the transport wants to send.
    bool Flush()
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        while (true) {
            const auto [bytes, more, type] = m_transport.GetBytesToSend(false);
            if (bytes.empty()) return true;
            const ssize_t n = send(m_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n <= 0) return false;
            m_transport.MarkBytesSent(static_cast<size_t>(n));
        }
    }

    bool Send(node::Sv2NetMsg msg)
    {
        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            CSerializedNetMsg net_msg{std::move(msg)};
            if (!m_transport.SetMessageToSend(net_msg)) return false;
        }
        return Flush();
    }

    //! Read from the socket until one message is complete (or handshake progresses).
    std::optional<node::Sv2NetMsg> Receive()
    {
        while (!m_transport.ReceivedMessageComplete()) {
            if (m_pending.empty()) {
                unsigned char buf[65536];
                const ssize_t n = recv(m_fd, buf, sizeof(buf), 0);
                if (n <= 0) return std::nullopt;
                m_pending.assign(buf, buf + n);
            }
            std::span<const uint8_t> span{m_pending.data(), m_pending.size()};
            if (!m_transport.ReceivedBytes(span)) return std::nullopt;
            m_pending.erase(m_pending.begin(), m_pending.end() - span.size());
            // Handshake replies are queued inside the transport; send them.
            if (!Flush()) return std::nullopt;
            if (m_transport.GetSendState() == Sv2Transport::SendState::READY && !m_ready) {
                m_ready = true;
                return std::nullopt; // signal "handshake complete" to the caller via Ready()
            }
        }
        bool reject{false};
        CNetMessage msg{m_transport.GetReceivedMessage(std::chrono::microseconds{0}, reject)};
        return node::Sv2NetMsg{std::move(msg)};
    }

    bool Ready() const { return m_ready; }

private:
    int m_fd;
    Sv2Transport m_transport;
    std::mutex m_send_mutex;
    std::vector<uint8_t> m_pending;
    bool m_ready{false};
};

node::Sv2NetMsg SetupConnection(const Options& opt)
{
    std::vector<uint8_t> bytes;
    VectorWriter w{bytes, 0};
    const auto str0_255 = [&](const std::string& s) {
        w << static_cast<uint8_t>(s.size());
        w.write(MakeByteSpan(s));
    };
    w << uint8_t{0x02} << uint16_t{2} << uint16_t{2} << uint32_t{0};
    str0_255(opt.host);
    w << static_cast<uint16_t>(std::stoi(opt.port));
    str0_255("ApertureCoin");
    str0_255("cpu");
    str0_255("aperture-sv2-miner");
    str0_255("0");
    return node::Sv2NetMsg{node::Sv2MsgType::SETUP_CONNECTION, std::move(bytes)};
}

Template ParseNewTemplate(const std::vector<uint8_t>& payload)
{
    SpanReader r{payload};
    Template t;
    uint8_t future;
    r >> t.id >> future >> t.version >> t.coinbase_version;
    t.future = future != 0;
    uint8_t prefix_len;
    r >> prefix_len;
    t.coinbase_prefix.resize(prefix_len);
    r.read(MakeWritableByteSpan(t.coinbase_prefix));
    uint32_t outputs_count;
    uint16_t outputs_len;
    r >> t.coinbase_sequence >> t.value_remaining >> outputs_count >> outputs_len;
    std::vector<uint8_t> outputs_blob(outputs_len);
    r.read(MakeWritableByteSpan(outputs_blob));
    SpanReader outputs_reader{outputs_blob};
    for (uint32_t i = 0; i < outputs_count; ++i) {
        CTxOut out;
        outputs_reader >> out;
        t.outputs.push_back(out);
    }
    r >> t.locktime >> t.merkle_path;
    return t;
}

bool IsWitnessCommitment(const CScript& script)
{
    return script.size() >= 38 && script[0] == OP_RETURN && script[1] == 0x24 && script[2] == 0xaa &&
           script[3] == 0x21 && script[4] == 0xa9 && script[5] == 0xed;
}

CMutableTransaction BuildCoinbase(const Template& t, const CScript& payout, uint64_t extranonce)
{
    CMutableTransaction cb;
    cb.version = t.coinbase_version;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    CScript script_sig(t.coinbase_prefix.begin(), t.coinbase_prefix.end());
    std::vector<unsigned char> en(8);
    for (int i = 0; i < 8; ++i) en[i] = (extranonce >> (8 * i)) & 0xff;
    script_sig << en;
    cb.vin[0].scriptSig = script_sig;
    cb.vin[0].nSequence = t.coinbase_sequence;
    cb.vout.emplace_back(static_cast<CAmount>(t.value_remaining), payout);
    bool has_commitment{false};
    for (const auto& out : t.outputs) {
        cb.vout.push_back(out);
        has_commitment |= IsWitnessCommitment(out.scriptPubKey);
    }
    // BIP141: the witness commitment was built with an all-zero reserved value.
    if (has_commitment) cb.vin[0].scriptWitness.stack.emplace_back(32, 0);
    cb.nLockTime = t.locktime;
    return cb;
}

uint256 MerkleRoot(const CMutableTransaction& coinbase, const std::vector<uint256>& path)
{
    uint256 root{CTransaction(coinbase).GetHash().ToUint256()};
    for (const uint256& sibling : path) root = Hash(root, sibling);
    return root;
}

/** Serialize and send SubmitUsefulWorkSolution (extension 0x4150, type 0x02). */
void SubmitV2(Connection& conn, const Job& job, const CBlockHeader& header, const matmulpow_v2::Ticket& t,
              const std::vector<int8_t>& panel, const CMutableTransaction& coinbase)
{
    std::vector<uint8_t> msg;
    VectorWriter w{msg, 0};
    w << job.tmpl.id << job.tmpl.version << header.nTime << header.nNonce << t.op << t.i << t.j << t.s
      << static_cast<uint16_t>(panel.size());
    w.write(std::as_bytes(std::span{panel}));
    std::vector<uint8_t> cb_bytes;
    VectorWriter{cb_bytes, 0, TX_WITH_WITNESS(coinbase)};
    w << static_cast<uint32_t>(cb_bytes.size());
    w.write(MakeByteSpan(cb_bytes));
    conn.Send(node::Sv2NetMsg{static_cast<node::Sv2MsgType>(node::APERTURE_SUBMIT_USEFUL_WORK), std::move(msg), node::SV2_EXT_APERTURE});
}

/**
 * ApertureMatMul v2: every r x r output tile of every weight matmul of the
 * forward pass over the block's requests is a lottery ticket; a new nonce
 * reseeds the noise (same search as the node's SolvePowV2).
 */
void MineV2(Connection& conn, const Options& opt, const Job& job, const CMutableTransaction& coinbase)
{
    const UsefulWork& uw{*job.tmpl.useful};
    CBlockHeader header;
    header.nVersion = static_cast<int32_t>(job.tmpl.version);
    header.hashPrevBlock = job.prev_hash;
    header.hashMerkleRoot = MerkleRoot(coinbase, job.tmpl.merkle_path);
    header.nTime = std::max<uint32_t>(job.ntime, static_cast<uint32_t>(time(nullptr)));
    header.nBits = job.nbits;
    std::vector<matmulpow_v2::OpInput> inputs;
    for (const intmodel::OpTrace& tr : uw.trace) inputs.push_back(matmulpow_v2::OpInput{tr.op, tr.rows, tr.q.data()});
    const uint256 target_le{ArithToUint256(job.target)};
    for (uint32_t nonce = 0; nonce < 0xffffffff && !g_stop; ++nonce) {
        if (g_generation.load() != job.generation) return;
        header.nNonce = nonce;
        std::vector<unsigned char> seed_input;
        VectorWriter{seed_input, 0, header};
        seed_input.insert(seed_input.end(), uw.batch_root.begin(), uw.batch_root.end());
        unsigned char sigma[32];
        matmulpow_v2::Seed(seed_input.data(), seed_input.size(), sigma);
        // One nonce = the noisy forward-pass matmuls; every r x r x r tile is a ticket.
        matmulpow_v2::SearchHit hit;
        uint64_t tickets{0};
        const bool found{matmulpow_v2::SearchNonce(g_backend, sigma, uw.rank, g_model->Ops(), inputs, target_le.begin(), hit, tickets)};
        g_hashes += tickets;
        if (!found) continue;
        uint64_t solved{g_solved_generation.load()};
        if (solved >= job.generation || !g_solved_generation.compare_exchange_strong(solved, job.generation)) return;
        SubmitV2(conn, job, header, hit.ticket, hit.panel, coinbase);
        const int n{++g_found};
        std::cout << "found useful-work block: template " << job.tmpl.id << " ticket op=" << hit.ticket.op << " i=" << hit.ticket.i
                  << " j=" << hit.ticket.j << " s=" << hit.ticket.s << " (" << n << " total)" << std::endl;
        if (opt.max_blocks > 0 && n >= opt.max_blocks) g_stop = true;
        while (!g_stop && g_generation.load() == job.generation) std::this_thread::sleep_for(std::chrono::milliseconds{10});
        return;
    }
}

/** Parse UsefulWorkTemplate and run the protocol model over its requests. */
std::optional<std::pair<uint64_t, std::shared_ptr<const UsefulWork>>> ParseUsefulWork(const std::vector<uint8_t>& payload)
{
    if (!g_model) {
        std::cerr << "v2 template received but no protocol model loaded (use -protocolmodel or -tinymodel)\n";
        return std::nullopt;
    }
    auto uw{std::make_shared<UsefulWork>()};
    uint64_t template_id;
    SpanReader r{payload};
    uw->batch_root.resize(32);
    uw->model_id.resize(32);
    uint8_t rank;
    uint16_t count;
    r >> template_id;
    r.read(MakeWritableByteSpan(uw->batch_root));
    r.read(MakeWritableByteSpan(uw->model_id));
    r >> rank >> count;
    uw->rank = rank;
    for (uint16_t k = 0; k < count; ++k) {
        uint16_t n;
        r >> n;
        std::vector<uint32_t> ids(n);
        for (auto& id : ids) {
            uint8_t b[3];
            r >> b[0] >> b[1] >> b[2];
            id = b[0] | (b[1] << 8) | (uint32_t{b[2]} << 16);
        }
        uw->requests.push_back(std::move(ids));
    }
    if (HexStr(uw->model_id) != g_model->Apm().ModelIdHex()) {
        std::cerr << "template model " << HexStr(uw->model_id) << " differs from the loaded model " << g_model->Apm().ModelIdHex() << "\n";
        return std::nullopt;
    }
    matmulpow_v2::SetModel(g_model->Ops(), uw->rank);
    // Forward pass over every request (the useful work); an empty batch runs EOS only.
    auto merge = [&](std::vector<intmodel::OpTrace>& part) {
        if (uw->trace.empty()) {
            uw->trace = std::move(part);
            return;
        }
        for (size_t k = 0; k < uw->trace.size(); ++k) {
            uw->trace[k].q.insert(uw->trace[k].q.end(), part[k].q.begin(), part[k].q.end());
            uw->trace[k].rows += part[k].rows;
        }
    };
    std::vector<std::vector<uint32_t>> inputs{uw->requests};
    if (inputs.empty()) inputs.emplace_back();
    for (auto ids : inputs) {
        ids.push_back(g_model->Config().eos_token_id);
        std::vector<intmodel::OpTrace> part;
        if (g_model->Embed(ids, &part).empty()) {
            std::cerr << "template request cannot be embedded\n";
            return std::nullopt;
        }
        merge(part);
    }
    std::cout << "useful work for template " << template_id << ": " << uw->requests.size() << " embedding requests, "
              << uw->trace.size() << " weight matmuls" << std::endl;
    return std::make_pair(template_id, std::shared_ptr<const UsefulWork>{uw});
}

void Mine(Connection& conn, const Options& opt, unsigned int thread_id)
{
    uint64_t extranonce{(static_cast<uint64_t>(thread_id) << 48) ^ FastRandomContext().rand64()};
    while (!g_stop) {
        std::optional<Job> job;
        {
            std::lock_guard<std::mutex> lock(g_job_mutex);
            job = g_job;
        }
        if (!job) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            continue;
        }
        const CMutableTransaction coinbase{BuildCoinbase(job->tmpl, opt.payout, ++extranonce)};
        if (job->tmpl.version & VERSION_POWV2) {
            if (!job->tmpl.useful) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            MineV2(conn, opt, *job, coinbase);
            continue;
        }
        CBlockHeader header;
        header.nVersion = static_cast<int32_t>(job->tmpl.version);
        header.hashPrevBlock = job->prev_hash;
        header.hashMerkleRoot = MerkleRoot(coinbase, job->tmpl.merkle_path);
        header.nTime = std::max<uint32_t>(job->ntime, static_cast<uint32_t>(time(nullptr)));
        header.nBits = job->nbits;

        for (uint32_t nonce = 0; nonce < 0xffffffff && !g_stop; ++nonce) {
            if ((nonce & 0x3f) == 0 && g_generation.load() != job->generation) break;
            header.nNonce = nonce;
            std::vector<unsigned char> ser;
            VectorWriter{ser, 0, header};
            unsigned char pow[32];
            matmulpow::Hash(ser.data(), opt.dim, pow);
            ++g_hashes;
            if (UintToArith256(uint256{std::span<const unsigned char>{pow, 32}}) > job->target) continue;
            // Only the first thread to solve this job submits.
            uint64_t solved{g_solved_generation.load()};
            if (solved >= job->generation || !g_solved_generation.compare_exchange_strong(solved, job->generation)) break;

            std::vector<uint8_t> msg;
            VectorWriter w{msg, 0};
            w << job->tmpl.id << job->tmpl.version << header.nTime << nonce;
            std::vector<uint8_t> cb_bytes;
            VectorWriter{cb_bytes, 0, TX_WITH_WITNESS(coinbase)};
            w << static_cast<uint16_t>(cb_bytes.size());
            w.write(MakeByteSpan(cb_bytes));
            conn.Send(node::Sv2NetMsg{node::Sv2MsgType::SUBMIT_SOLUTION, std::move(msg)});
            const int found{++g_found};
            std::cout << "found block: template " << job->tmpl.id << " nonce " << nonce << " (" << found << " total)" << std::endl;
            if (opt.max_blocks > 0 && found >= opt.max_blocks) g_stop = true;
            // Wait for the next SetNewPrevHash instead of mining a stale job.
            while (!g_stop && g_generation.load() == job->generation) std::this_thread::sleep_for(std::chrono::milliseconds{10});
            break;
        }
    }
}

void Usage()
{
    std::cerr << "usage: aperture-sv2-miner -connect=<host:port> -authority=<base58 key> [-payout=<scriptPubKey hex>]\n"
                 "                          [-dim=<n>] [-threads=<n>] [-blocks=<n>]\n"
                 "  -dim      ApertureMatMul matrix dimension: 512 (main/test), 32 (regtest)\n"
                 "  -payout   scriptPubKey for the reward (default: OP_TRUE, for testing only)\n"
                 "  -protocolmodel=<file.apm> / -tinymodel   protocol model for ApertureMatMul v2 templates\n"
                 "  -kernel=scalar|avx512-vnni   v2 mining kernel (default: fastest available)\n";
}

} // namespace

int main(int argc, char** argv)
{
    ECC_Context ecc_context{};
    SHA256AutoDetect();
    RandomInit();
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        const auto eq = arg.find('=');
        const std::string key{arg.substr(0, eq)}, val{eq == std::string::npos ? "" : arg.substr(eq + 1)};
        if (key == "-connect") {
            const auto colon = val.rfind(':');
            opt.host = val.substr(0, colon);
            if (colon != std::string::npos) opt.port = val.substr(colon + 1);
        } else if (key == "-authority") {
            opt.authority = val;
        } else if (key == "-payout") {
            const auto bytes{ParseHex(val)};
            opt.payout = CScript(bytes.begin(), bytes.end());
        } else if (key == "-dim") {
            opt.dim = static_cast<unsigned int>(std::stoul(val));
        } else if (key == "-threads") {
            opt.threads = static_cast<unsigned int>(std::stoul(val));
        } else if (key == "-blocks") {
            opt.max_blocks = std::stoi(val);
        } else if (key == "-protocolmodel") {
            opt.model_path = val;
        } else if (key == "-tinymodel") {
            opt.tiny_model = true;
        } else if (key == "-kernel") {
            g_backend = val == "scalar" ? matmulpow_v2::Backend::SCALAR : matmulpow_v2::Backend::AVX512_VNNI;
            if (!matmulpow_v2::BackendAvailable(g_backend)) {
                std::cerr << "kernel " << val << " is not available on this CPU\n";
                return 1;
            }
        } else {
            Usage();
            return 1;
        }
    }
    std::vector<unsigned char> authority_bytes;
    if (!DecodeBase58Check(opt.authority, authority_bytes, 34) || authority_bytes.size() != 34 || !matmulpow::IsValidDim(opt.dim)) {
        Usage();
        return 1;
    }
    const XOnlyPubKey authority{std::span<const unsigned char>{authority_bytes}.subspan(2)};

    if (!opt.model_path.empty() || opt.tiny_model) {
        auto apm_model{std::make_unique<apm::Model>()};
        std::string error;
        const bool loaded{opt.model_path.empty() ? apm_model->Load(intmodel::BuildTinyModel(1), error)
                                                 : apm_model->LoadFile(opt.model_path, error)};
        g_model = std::make_unique<intmodel::IntModel>();
        if (!loaded || !g_model->Init(std::move(apm_model), error)) {
            std::cerr << "cannot load protocol model: " << error << "\n";
            return 1;
        }
        std::cout << "protocol model " << g_model->Apm().ModelIdHex() << ", kernel " << matmulpow_v2::BackendName(g_backend) << std::endl;
    }

    const int fd{Connect(opt)};
    if (fd < 0) {
        std::cerr << "cannot connect to " << opt.host << ":" << opt.port << "\n";
        return 1;
    }
    Connection conn{fd, authority};
    if (!conn.Flush()) return 1; // handshake step 1
    while (!conn.Ready()) {
        if (!conn.Receive() && !conn.Ready()) {
            std::cerr << "handshake failed\n";
            return 1;
        }
    }
    conn.Send(SetupConnection(opt));
    {
        std::vector<uint8_t> bytes;
        VectorWriter{bytes, 0, uint32_t{64}, uint16_t{4}}; // room for one payout output
        conn.Send(node::Sv2NetMsg{node::Sv2MsgType::COINBASE_OUTPUT_CONSTRAINTS, std::move(bytes)});
    }

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < opt.threads; ++i) workers.emplace_back(Mine, std::ref(conn), std::cref(opt), i);
    std::thread stats([] {
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::seconds{10});
            std::cout << "hashrate: " << g_hashes.exchange(0) / 10.0 << " H/s" << std::endl;
        }
    });

    std::map<uint64_t, Template> templates;
    std::optional<Template> latest;
    while (!g_stop) {
        auto msg{conn.Receive()};
        if (!msg) {
            if (g_stop) break;
            std::cerr << "connection closed\n";
            g_stop = true;
            break;
        }
        std::vector<uint8_t> payload{msg->m_msg};
        if (msg->m_extension_type == node::SV2_EXT_APERTURE) {
            if (uint8_t(msg->m_msg_type) != node::APERTURE_USEFUL_WORK_TEMPLATE) continue;
            const auto uw{ParseUsefulWork(payload)};
            if (!uw) continue;
            auto it{templates.find(uw->first)};
            if (it != templates.end()) it->second.useful = uw->second;
            std::lock_guard<std::mutex> lock(g_job_mutex);
            if (g_job && g_job->tmpl.id == uw->first) {
                g_job->tmpl.useful = uw->second;
                g_job->generation = ++g_generation;
            }
            continue;
        }
        switch (msg->m_msg_type) {
        case node::Sv2MsgType::SETUP_CONNECTION_SUCCESS:
            std::cout << "connected to template provider" << std::endl;
            break;
        case node::Sv2MsgType::SETUP_CONNECTION_ERROR:
            std::cerr << "SetupConnection rejected\n";
            g_stop = true;
            break;
        case node::Sv2MsgType::NEW_TEMPLATE: {
            Template t{ParseNewTemplate(payload)};
            templates[t.id] = t;
            if (!t.future) {
                // Applies to the current prev hash immediately.
                std::lock_guard<std::mutex> lock(g_job_mutex);
                if (g_job) {
                    g_job->tmpl = t;
                    g_job->generation = ++g_generation;
                }
            }
            break;
        }
        case node::Sv2MsgType::SET_NEW_PREV_HASH: {
            SpanReader r{payload};
            uint64_t template_id;
            uint256 prev_hash, target;
            uint32_t ntime, nbits;
            r >> template_id >> prev_hash >> ntime >> nbits >> target;
            const auto it{templates.find(template_id)};
            if (it == templates.end()) break;
            std::lock_guard<std::mutex> lock(g_job_mutex);
            g_job = Job{++g_generation, it->second, prev_hash, ntime, nbits, UintToArith256(target)};
            std::cout << "new block template " << template_id << " on " << prev_hash.GetHex().substr(0, 16) << std::endl;
            break;
        }
        default:
            break;
        }
    }
    g_stop = true;
    for (auto& w : workers) w.join();
    stats.join();
    close(fd);
    return g_found > 0 || opt.max_blocks <= 0 ? 0 : 2;
}
