#pragma once
#include <cstdint>

// =====================================================================
// plaintext_log.h — "read the client's own plaintext" thread
// (docs/investigation/CONNECTION_DECIPHER_PREP.md). Persists PLAINTEXT --
// pre-cipher outbound, and (once wired) post-decrypt inbound -- with a
// per-process sequence number and the exact wire size passed to SendMsg,
// so entries can be aligned against relay_packets.log's ciphertext
// captures for the same session and cross-checked for a size match.
//
// A size MATCH between a LogOutbound() entry and the corresponding
// relay-observed ciphertext chunk confirms the "no framing added
// downstream of SendMsg" model (v1074 analysis: the enqueue copies bytes
// verbatim, cipher applies in place) -- and, since the cipher is confirmed
// static-start / positional (CONNECTION_DECIPHER_PREP.md's multi-connection
// diff finding), a matched pair also directly yields that stretch of
// keystream via plaintext XOR ciphertext, as a byproduct, not a goal.
// A size MISMATCH means framing/merging happens between SendMsg and the
// socket -- see tools/plaintext_ciphertext_align.py, which does this
// alignment/cross-check offline against a captured pair of logs and flags
// exactly this rather than silently assuming a wrong 1:1 correspondence.
// =====================================================================
namespace PlaintextLog {

// Opens plaintext_<pid>.log next to the other per-pid logs (same directory/naming
// convention as Log::Init). Safe to call even if it fails to open -- LogOutbound
// becomes a no-op rather than crashing anything.
void Init();
void Shutdown();

// `data`/`wireSize` are EXACTLY what was passed into CNetClient::SendMsg (real) --
// i.e. HkSendMsgReal's own arguments, before the native function (and whatever cipher
// it calls into) touches them. Labels by msg_types.h when the 4-byte [size][type]
// header is present and readable.
void LogOutbound(const uint8_t* data, uint32_t wireSize);

// Wired in once the message-dispatch function (post-decrypt consumer of inbound data)
// is located -- see docs/investigation/CONNECTION_DECIPHER_PREP.md's offline
// dispatcher-signature scan. Not called anywhere yet.
void LogInbound(const uint8_t* data, uint32_t size);

} // namespace PlaintextLog
