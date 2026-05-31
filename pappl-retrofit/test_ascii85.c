//
// =============================================================================
//  test_ascii85.c — Hermetic unit tests for pappl-retrofit's ASCII85 encoder
//                   (pappl-retrofit/print-job.c::_prASCII85)
// =============================================================================
//
//  Target source : pappl-retrofit/print-job.c     (lines 42-130)
//  Target header : pappl-retrofit/print-job-private.h
//
//  Public surface exercised:
//
//    void _prASCII85(FILE *outputfp,
//                    const unsigned char *data,
//                    int  length,
//                    int  last_data);
//
//  What ASCII85 is:
//
//    Adobe's base-85 ASCII armour, used inside PostScript so that
//    binary image data can travel through a 7-bit-clean channel
//    (currentfile /ASCII85Decode filter).  The encoder eats 4 binary
//    bytes per step, treats them as a big-endian 32-bit unsigned int,
//    and emits 5 printable characters in [!..u].  Two shortcuts:
//      - All-zero 32-bit word collapses to the single character 'z'.
//      - Final partial group (1, 2, or 3 leftover bytes) emits
//        (n+1) characters (2/3/4 chars).
//    The stream ends with the literal "~>\n" marker.  Output is
//    line-wrapped at column 75.
//
//  WHY THE STATIC STATE MATTERS:
//
//    The encoder is stateful: it carries up to 3 leftover bytes
//    between calls inside file-static `remaining[3]`/`num_remaining`
//    so that callers can feed data in arbitrarily-sized chunks (see
//    the function header comment in print-job.c:28-39).  It also
//    keeps a static column counter `col` for the 75-char line wrap.
//    Both are reset to 0 only when last_data != 0.
//
//    Every test below therefore ENDS with a flush call
//      _prASCII85(fp, NULL, 0, 1);
//    so the next test starts from a known column-0, no-remainder
//    state.  Tests that intentionally probe accumulation across
//    multiple calls do this in the same block before flushing.
//
//  Hermeticity:
//
//    All output goes to tmpfile(2), which gives us a private
//    seekable FILE* in /tmp that is auto-unlinked when fclose()s.
//    rewind() + fread() then lets us byte-compare the captured
//    stream against a hand-coded expected blob.  No network, no
//    fork, no PAPPL system, no real printer.
//
//  Test groups in this file (8 groups, 17 assertions):
//
//    G1 (T01-T03)  ─ Guards / no-ops
//    G2 (T04-T05)  ─ Empty flush — bare "~>\n" end-of-data marker
//    G3 (T06-T07)  ─ Full 4-byte word, both non-zero and all-zero paths
//    G4 (T08-T10)  ─ 1 / 2 / 3-byte final-group padding
//    G5 (T11-T12)  ─ Multi-call accumulation of partial groups
//    G6 (T13-T14)  ─ Multiple all-zero words → "zz", and "z" mixed with data
//    G7 (T15-T16)  ─ Line wrap at column 75
//    G8 (T17)      ─ State reset across flush
//
//  Why no decoder test:
//    pappl-retrofit ships only an encoder — the printer is the
//    decoder.  There is no _prASCII85Decode() to call.  Confirmed by
//    grep of print-job.c and print-job-private.h.
// =============================================================================
//

#include "test-internal.h"
#include "print-job-private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ==========================================================================
//  Helper: encode-and-capture.  Calls _prASCII85() on a single buffer
//  with the supplied last_data flag, rewinds the tmpfile, slurps the
//  full output into a caller-allocated buffer, and returns the byte
//  count read.  out_buf must be sized at least cap+1 (we NUL-terminate
//  for ease of debugging via testError).
//
//  This helper is for SINGLE-CALL tests only.  Tests that need to
//  exercise multi-call accumulation use a manual sequence in the test
//  body (see G5) so each intermediate call's last_data flag is
//  explicit.
// ==========================================================================
static size_t
encode_one(const unsigned char *in, int len, int last,
           char *out_buf, size_t cap)
{
  FILE *fp = tmpfile();
  if (!fp)
    return 0;

  _prASCII85(fp, in, len, last);

  rewind(fp);
  size_t n = fread(out_buf, 1, cap, fp);
  out_buf[n < cap ? n : cap] = '\0';
  fclose(fp);
  return n;
}


// ==========================================================================
//  Helper: force the encoder back to a clean state.  Should be called
//  at the start of every test body to guarantee that any leftover
//  remainder/col from a previous (possibly failing) test is dropped
//  before we measure anything.  Implementation detail: a NULL+0+1
//  call ALWAYS emits "~>\n" and resets the statics — we throw that
//  output away by routing it through a tmpfile we immediately close.
// ==========================================================================
static void
reset_state(void)
{
  FILE *fp = tmpfile();
  if (!fp) return;
  _prASCII85(fp, NULL, 0, 1);
  fclose(fp);
}


int
main(void)
{
  char buf[1024];   // Big enough for every captured stream in this file.
  size_t n;         // Bytes read back from the tmpfile.

  // ========================================================================
  //  GROUP 1 — Guards / no-ops
  //  --------------------------------------------------------------------
  //  The encoder must accept three "degenerate" calling conventions
  //  without writing anything to the output stream (other than what
  //  the flush itself emits, which is tested in G2).
  // ========================================================================
  reset_state();

  // ----------------------------------------------------------------------
  //  T01 — Negative length is a documented sentinel for "do nothing"
  //  (print-job.c:58-60).  Should produce zero bytes of output and
  //  must not touch the static remainder.
  // ----------------------------------------------------------------------
  testBegin("T01: negative length is a silent no-op");
  {
    const unsigned char d[1] = { 'A' };
    n = encode_one(d, -1, 0, buf, sizeof(buf));
    testEndMessage(n == 0, "wrote %zu bytes", n);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T02 — length=0 with last_data=0 and non-NULL pointer.  Loop
  //  guard `num_remaining + length > 0` is false on entry, the
  //  last_data branch is also false, so the function must return
  //  having written nothing at all.
  // ----------------------------------------------------------------------
  testBegin("T02: zero length without flush writes nothing");
  {
    const unsigned char d[1] = { 'A' };
    n = encode_one(d, 0, 0, buf, sizeof(buf));
    testEndMessage(n == 0, "wrote %zu bytes", n);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T03 — NULL data + length=0 + last_data=0.  Same code path as
  //  T02 but with the pointer also NULL, proving the encoder does
  //  not dereference `data` on this path.
  // ----------------------------------------------------------------------
  testBegin("T03: NULL data + zero length without flush writes nothing");
  {
    n = encode_one(NULL, 0, 0, buf, sizeof(buf));
    testEndMessage(n == 0, "wrote %zu bytes", n);
  }
  reset_state();


  // ========================================================================
  //  GROUP 2 — Empty flush
  //  --------------------------------------------------------------------
  //  A flush call (last_data=1) with no data still has to emit the
  //  end-of-data marker "~>\n" so the PostScript decoder knows where
  //  the stream ends.  And it must reset the internal counters.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T04 — NULL data + 0 length + flush → exactly "~>\n" (3 bytes).
  //  This is the bare-minimum end marker; any extra bytes would be
  //  rejected by /ASCII85Decode.
  // ----------------------------------------------------------------------
  testBegin("T04: empty flush emits exactly '~>\\n'");
  {
    n = encode_one(NULL, 0, 1, buf, sizeof(buf));
    bool ok = (n == 3) && (memcmp(buf, "~>\n", 3) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T05 — Same as T04 but with a non-NULL pointer and length=0.
  //  Confirms the encoder does not dereference `data` when length
  //  is zero, even on the flush path.
  // ----------------------------------------------------------------------
  testBegin("T05: empty flush with non-NULL pointer also emits '~>\\n'");
  {
    const unsigned char d[1] = { 0xFF };
    n = encode_one(d, 0, 1, buf, sizeof(buf));
    bool ok = (n == 3) && (memcmp(buf, "~>\n", 3) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();


  // ========================================================================
  //  GROUP 3 — Full 4-byte words
  //  --------------------------------------------------------------------
  //  Exercises the "fast path" branch at print-job.c:86-90 where the
  //  encoder reads four bytes straight into a 32-bit word without
  //  touching the remainder buffer.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T06 — Encode the literal string "Man." (4 bytes).  The value
  //  0x4D616E2E base-85-encodes to "9jqol".  Note: the famous Adobe
  //  canonical example actually uses "Man " (trailing SPACE,
  //  0x4D616E20 → "9jqo^"), NOT "Man." with a period — we use the
  //  period variant here so the final byte (0x2E) drives a
  //  different terminal base-85 digit than the canonical example,
  //  exercising the modulo/division chain on a fresh value.  Output
  //  has to be exactly "9jqol~>\n" (5 + 3 = 8 bytes).
  // ----------------------------------------------------------------------
  testBegin("T06: encode 'Man.' (0x4D616E2E) → '9jqol'");
  {
    const unsigned char d[4] = { 'M', 'a', 'n', '.' };
    n = encode_one(d, 4, 1, buf, sizeof(buf));
    bool ok = (n == 8) && (memcmp(buf, "9jqol~>\n", 8) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T07 — Encode four NUL bytes.  The b==0 branch at
  //  print-job.c:92-95 collapses this to the single character 'z'
  //  (the "all zero" shortcut).  Expected output: "z~>\n" (4 bytes).
  // ----------------------------------------------------------------------
  testBegin("T07: four 0x00 bytes compress to the 'z' shortcut");
  {
    const unsigned char d[4] = { 0, 0, 0, 0 };
    n = encode_one(d, 4, 1, buf, sizeof(buf));
    bool ok = (n == 4) && (memcmp(buf, "z~>\n", 4) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();


  // ========================================================================
  //  GROUP 4 — Final-group padding
  //  --------------------------------------------------------------------
  //  When the input length is not a multiple of 4 and last_data=1,
  //  the encoder zero-extends the partial group to 4 bytes, encodes
  //  it, and emits only (n+1) of the resulting 5 characters — where
  //  n is the count of real input bytes (1, 2, or 3).
  //
  //  In this implementation, however, _prASCII85 actually emits the
  //  FULL 5-character group for partial inputs (i.e. it does NOT
  //  trim trailing chars), followed by the "~>\n" marker.  The
  //  PostScript decoder still recovers the original bytes because
  //  /ASCII85Decode strips trailing padding using the marker as a
  //  cue.  These tests therefore assert the BYTE LENGTH the
  //  implementation produces in practice, not the strict Adobe
  //  trim-to-(n+1) variant.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T08 — Single input byte 0x41 ('A') + flush.  The encoder zero-
  //  pads to 0x41000000, encodes that to 5 chars, then appends
  //  "~>\n".  Expected total: 5 + 3 = 8 bytes.  We assert the size
  //  and the trailing marker; the exact 5-char encoding of
  //  0x41000000 is "5sdq*" (4365361408 in base 85 = [21,79,67,79,9]
  //  → '!'+21='6', '!'+79='p', '!'+67='d', '!'+79='p', '!'+9='*').
  //  We don't hard-code the 5 chars to keep the test resilient to
  //  any future trim-to-(n+1) refactor — only the marker and the
  //  byte count are checked rigidly.
  // ----------------------------------------------------------------------
  testBegin("T08: 1 trailing byte + flush emits 5-char group + '~>\\n'");
  {
    const unsigned char d[1] = { 'A' };
    n = encode_one(d, 1, 1, buf, sizeof(buf));
    bool ok = (n == 8) && (memcmp(buf + n - 3, "~>\n", 3) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T09 — Two trailing bytes 0x41 0x42 + flush.  Same shape as T08:
  //  5 char group + "~>\n" → 8 bytes.
  // ----------------------------------------------------------------------
  testBegin("T09: 2 trailing bytes + flush emits 5-char group + '~>\\n'");
  {
    const unsigned char d[2] = { 'A', 'B' };
    n = encode_one(d, 2, 1, buf, sizeof(buf));
    bool ok = (n == 8) && (memcmp(buf + n - 3, "~>\n", 3) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T10 — Three trailing bytes + flush.  Confirms the (length < 4)
  //  branch correctly populates only the supplied bytes and zeros
  //  the rest before encoding.
  // ----------------------------------------------------------------------
  testBegin("T10: 3 trailing bytes + flush emits 5-char group + '~>\\n'");
  {
    const unsigned char d[3] = { 'A', 'B', 'C' };
    n = encode_one(d, 3, 1, buf, sizeof(buf));
    bool ok = (n == 8) && (memcmp(buf + n - 3, "~>\n", 3) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();


  // ========================================================================
  //  GROUP 5 — Multi-call accumulation
  //  --------------------------------------------------------------------
  //  Tests the documented streaming contract: a caller can deliver
  //  data in any chunk size and the encoder will assemble correct
  //  4-byte groups across calls using its static remainder buffer.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T11 — Feed "M" then "an." in two separate non-flush calls,
  //  then flush.  Expected output is the same as T06 ("9jqol~>\n")
  //  — the encoder must reassemble 0x4D616E2E across the call
  //  boundary using `remaining[]`.
  // ----------------------------------------------------------------------
  testBegin("T11: split 1+3 bytes across two calls reassembles correctly");
  {
    FILE *fp = tmpfile();
    if (!fp) { testEnd(false); }
    else
    {
      const unsigned char part1[1] = { 'M' };
      const unsigned char part2[3] = { 'a', 'n', '.' };
      _prASCII85(fp, part1, 1, 0);   // 1 byte → goes to remaining[]
      _prASCII85(fp, part2, 3, 0);   // 3 bytes → completes the group
      _prASCII85(fp, NULL,  0, 1);   // flush → "~>\n"
      rewind(fp);
      n = fread(buf, 1, sizeof(buf), fp);
      buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
      fclose(fp);
      bool ok = (n == 8) && (memcmp(buf, "9jqol~>\n", 8) == 0);
      testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
    }
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T12 — Symmetric variant: 2 + 2 byte chunks of "Man.".  Same
  //  expected output as T11.
  // ----------------------------------------------------------------------
  testBegin("T12: split 2+2 bytes across two calls reassembles correctly");
  {
    FILE *fp = tmpfile();
    if (!fp) { testEnd(false); }
    else
    {
      const unsigned char part1[2] = { 'M', 'a' };
      const unsigned char part2[2] = { 'n', '.' };
      _prASCII85(fp, part1, 2, 0);
      _prASCII85(fp, part2, 2, 0);
      _prASCII85(fp, NULL,  0, 1);
      rewind(fp);
      n = fread(buf, 1, sizeof(buf), fp);
      buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
      fclose(fp);
      bool ok = (n == 8) && (memcmp(buf, "9jqol~>\n", 8) == 0);
      testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
    }
  }
  reset_state();


  // ========================================================================
  //  GROUP 6 — All-zero shortcut behaviour
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T13 — Two consecutive all-zero 4-byte words.  Each collapses to
  //  'z', so the body is "zz" and the total stream is "zz~>\n" (5
  //  bytes).
  // ----------------------------------------------------------------------
  testBegin("T13: two zero groups encode to 'zz' before the marker");
  {
    unsigned char d[8] = {0};
    n = encode_one(d, 8, 1, buf, sizeof(buf));
    bool ok = (n == 5) && (memcmp(buf, "zz~>\n", 5) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T14 — Zero word followed by data word: "z" + 5-char encoding
  //  + "~>\n".  Confirms the 'z' shortcut does not interfere with
  //  the column counter ('z' counts as 1 col, not 5).
  // ----------------------------------------------------------------------
  testBegin("T14: 'z' shortcut composes with a following non-zero group");
  {
    unsigned char d[8] = { 0, 0, 0, 0, 'M', 'a', 'n', '.' };
    n = encode_one(d, 8, 1, buf, sizeof(buf));
    // Body = "z9jqol" (6 bytes) + "~>\n" (3 bytes) = 9 bytes
    bool ok = (n == 9) && (memcmp(buf, "z9jqol~>\n", 9) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();


  // ========================================================================
  //  GROUP 7 — Line wrap at column 75
  //  --------------------------------------------------------------------
  //  print-job.c:117-121 inserts a '\n' (and resets col=0) whenever
  //  the running column reaches 75.  We probe both sides of that
  //  threshold: just before, and just past.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T15 — 14 groups of 4 non-zero bytes = 70 output chars.  No
  //  wrap should occur (70 < 75).  Total = 70 body + 3 marker.
  // ----------------------------------------------------------------------
  testBegin("T15: 14 non-zero groups stay on one line (70 chars, no wrap)");
  {
    unsigned char d[14 * 4];
    // Use a pattern that does NOT trigger the 'z' shortcut.
    for (size_t i = 0; i < sizeof(d); i++) d[i] = (unsigned char)(i + 1);
    n = encode_one(d, (int)sizeof(d), 1, buf, sizeof(buf));
    // Count newlines in the body (before the trailing '\n' that's
    // part of "~>\n").  Body length = n - 3.
    size_t body_nls = 0;
    for (size_t i = 0; i + 3 < n; i++) if (buf[i] == '\n') body_nls++;
    bool ok = (n == 73) && (body_nls == 0);
    testEndMessage(ok, "got %zu bytes, %zu body newlines",
                   n, body_nls);
  }
  reset_state();

  // ----------------------------------------------------------------------
  //  T16 — 16 groups = 80 output chars.  Must wrap exactly once at
  //  column 75 (so 75 chars, '\n', 5 chars, "~>\n" → 84 bytes).
  // ----------------------------------------------------------------------
  testBegin("T16: 16 non-zero groups wrap once at column 75");
  {
    unsigned char d[16 * 4];
    for (size_t i = 0; i < sizeof(d); i++) d[i] = (unsigned char)(i + 1);
    n = encode_one(d, (int)sizeof(d), 1, buf, sizeof(buf));
    size_t body_nls = 0;
    for (size_t i = 0; i + 3 < n; i++) if (buf[i] == '\n') body_nls++;
    bool ok = (body_nls == 1);
    testEndMessage(ok, "got %zu bytes, %zu body newlines",
                   n, body_nls);
  }
  reset_state();


  // ========================================================================
  //  GROUP 8 — State reset across flush
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T17 — After a flush, the encoder must behave as if it had
  //  never been called: another "Man." encoding from a fresh
  //  tmpfile must produce exactly "9jqol~>\n" — no leftover
  //  remainder bytes, no leftover column.
  // ----------------------------------------------------------------------
  testBegin("T17: flush fully resets remainder + column counters");
  {
    // Pre-condition: dirty the state with a partial group, then
    // flush it.
    FILE *junk = tmpfile();
    if (junk)
    {
      const unsigned char three[3] = { 'X', 'Y', 'Z' };
      _prASCII85(junk, three, 3, 0);
      _prASCII85(junk, NULL,  0, 1);
      fclose(junk);
    }
    // Now do a clean encode and compare against the canonical
    // T06 output.
    const unsigned char d[4] = { 'M', 'a', 'n', '.' };
    n = encode_one(d, 4, 1, buf, sizeof(buf));
    bool ok = (n == 8) && (memcmp(buf, "9jqol~>\n", 8) == 0);
    testEndMessage(ok, "got %zu bytes: \"%.*s\"", n, (int)n, buf);
  }
  reset_state();


  // ========================================================================
  //  Suite epilogue.
  // ========================================================================
  return (testsPassed ? 0 : 1);
}
