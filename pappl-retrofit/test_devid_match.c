//
// =============================================================================
//  test_devid_match.c — Hermetic unit tests for pappl-retrofit's
//                       IEEE-1284 device-ID matching helpers
//                       (pappl-retrofit/pappl-retrofit.c)
// =============================================================================
//
//  Target source : pappl-retrofit/pappl-retrofit.c
//  Target header : pappl-retrofit/pappl-retrofit.h
//
//  Public surface exercised:
//
//    int  prRegExMatchDevIDField (const char *device_id,
//                                 const char *key,
//                                 const char *value_regex,
//                                 pr_devid_regex_mode_t mode);
//    bool prSupportsPostScript   (const char *device_id);
//    bool prSupportsPDF          (const char *device_id);
//    bool prSupportsPCL5         (const char *device_id);
//    bool prSupportsPCL5c        (const char *device_id);
//    bool prSupportsPCLXL        (const char *device_id);
//
//  WHAT AN IEEE-1284 DEVICE ID LOOKS LIKE:
//
//    A semicolon-separated list of "KEY:VALUE;" pairs, e.g.
//
//      "MFG:HP;MDL:LaserJet 4000;CMD:PCL,PS,PDF;CLS:PRINTER;"
//
//    KEY is the field name (MFG, MDL, CMD, CLS, COMMAND SET, ...)
//    and VALUE is a string that may itself be a comma-separated list
//    of items (e.g. CMD's PDL list).  Parsing is delegated to PAPPL's
//    papplDeviceParseID() (called from pappl-retrofit.c:324), so this
//    test file does NOT exercise the parser itself — it exercises
//    pappl-retrofit's MATCHING layer on top of the parser.
//
//  WHAT prSupports*() ACTUALLY DO:
//
//    Each prSupports*() helper boils down to a pair of
//    prRegExMatchDevIDField() calls — one against the "CMD" field
//    and one against the "COMMAND SET" field — using a PDL-specific
//    POSIX-extended, case-insensitive regex.  Returns true if either
//    field matches at least one comma-separated item.
//
//    Regexes (verbatim from pappl-retrofit.c:390-461):
//      PostScript : "^(POSTSCRIPT|BRSCRIPT|PS$|PS2$|PS3$)"
//      PDF        : "^(PDF)"
//      PCL 5      : "^(PCL([ -]?5([ -]?[ce])?)?)$"
//      PCL 5c     : "^(PCL[ -]?5[ -]?c)$"
//      PCL-XL     : "^(PCL[ -]?XL|PXL|PCL[ -]?6)$"
//
//  RETURN-VALUE CONVENTION OF prRegExMatchDevIDField():
//
//    >0  — number of matching items in the field value
//     0  — no item matched
//    -1  — field (key) not present in the parsed device ID
//    -2  — papplDeviceParseID() returned 0 / NULL
//    -3  — regex compilation failed
//    -5  — memory allocation failure
//   <-1 — other internal error
//
//    Therefore "matches" tests assert > 0, "no-match" tests assert
//    == 0, "key missing" tests assert == -1.
//
//  Test groups in this file (5 groups, 21 assertions):
//
//    G1 (T01-T05) ─ prRegExMatchDevIDField  primitive
//    G2 (T06-T10) ─ prSupportsPostScript
//    G3 (T11-T13) ─ prSupportsPDF
//    G4 (T14-T17) ─ prSupportsPCL5 + PCL5c
//    G5 (T18-T21) ─ prSupportsPCLXL + cross-PDL negatives
//
//  Hermeticity:
//    Pure-string-in / int-or-bool-out functions.  No filesystem, no
//    PAPPL system, no driver list (prBestMatchingPPD is intentionally
//    out of scope because it needs a fully-populated
//    pr_printer_app_global_data_t — that test would not be hermetic).
// =============================================================================
//

#include "test-internal.h"
#include "pappl-retrofit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int
main(void)
{
  // ========================================================================
  //  GROUP 1 — prRegExMatchDevIDField primitive
  //  --------------------------------------------------------------------
  //  Every prSupports*() helper is a thin wrapper over this function,
  //  so any coverage we get here transitively protects all 5 PDL
  //  detectors against regressions in the matching layer.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T01 — NULL device_id is documented as a hard error (the function
  //  early-outs at pappl-retrofit.c:319 before papplDeviceParseID()
  //  is called).  Expected return: a negative sentinel (we accept
  //  any < 0 rather than hard-coding -2/-5 so the test survives a
  //  future tightening of the error code).
  // ----------------------------------------------------------------------
  testBegin("T01: prRegExMatchDevIDField(NULL, ...) returns a negative error");
  {
    int r = prRegExMatchDevIDField(NULL, "CMD", "^PS$",
                                   PR_DEVID_REGEX_MATCH_ITEM);
    testEndMessage(r < 0, "ret=%d", r);
  }

  // ----------------------------------------------------------------------
  //  T02 — NULL key is also a hard error (same early-out block).
  // ----------------------------------------------------------------------
  testBegin("T02: prRegExMatchDevIDField(..., NULL key, ...) returns negative");
  {
    int r = prRegExMatchDevIDField("MFG:HP;MDL:LJ;", NULL, "^PS$",
                                   PR_DEVID_REGEX_MATCH_ITEM);
    testEndMessage(r < 0, "ret=%d", r);
  }

  // ----------------------------------------------------------------------
  //  T03 — Well-formed device_id but the requested key is not
  //  present.  Documented return: -1.  We accept any < 0 to stay
  //  resilient to error-code shuffling.
  // ----------------------------------------------------------------------
  testBegin("T03: key absent from device_id → negative return");
  {
    int r = prRegExMatchDevIDField("MFG:HP;MDL:LaserJet;", "CMD",
                                   "^PS$", PR_DEVID_REGEX_MATCH_ITEM);
    testEndMessage(r < 0, "ret=%d", r);
  }

  // ----------------------------------------------------------------------
  //  T04 — MATCH_ITEM mode: regex matches one of three comma-
  //  separated items in CMD.  Must return >0 (count of matches),
  //  not just non-zero.
  // ----------------------------------------------------------------------
  testBegin("T04: MATCH_ITEM finds 'PS' in CMD:PCL,PS,PDF");
  {
    int r = prRegExMatchDevIDField("MFG:HP;MDL:LJ;CMD:PCL,PS,PDF;",
                                   "CMD", "^PS$",
                                   PR_DEVID_REGEX_MATCH_ITEM);
    testEndMessage(r > 0, "ret=%d (expected >0 match count)", r);
  }

  // ----------------------------------------------------------------------
  //  T05 — MATCH_WHOLE_VALUE mode: regex anchored to the entire
  //  field value, not individual items.  "PCL,PS,PDF" does not
  //  match "^PS$" as a whole string, so this must return 0.
  // ----------------------------------------------------------------------
  testBegin("T05: MATCH_WHOLE_VALUE rejects per-item match on multi-item field");
  {
    int r = prRegExMatchDevIDField("MFG:HP;MDL:LJ;CMD:PCL,PS,PDF;",
                                   "CMD", "^PS$",
                                   PR_DEVID_REGEX_MATCH_WHOLE_VALUE);
    testEndMessage(r == 0, "ret=%d (expected 0 — whole value is 'PCL,PS,PDF')",
                   r);
  }


  // ========================================================================
  //  GROUP 2 — prSupportsPostScript
  //  --------------------------------------------------------------------
  //  Regex: "^(POSTSCRIPT|BRSCRIPT|PS$|PS2$|PS3$)"
  //  Fields searched: CMD and COMMAND SET, both in MATCH_ITEM mode.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T06 — Bare "PS" in CMD list → matches the "PS$" alternative.
  // ----------------------------------------------------------------------
  testBegin("T06: prSupportsPostScript(CMD with PS) → true");
  {
    bool r = prSupportsPostScript("MFG:HP;MDL:LJ;CMD:PCL,PS,PDF;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T07 — "POSTSCRIPT" literal in CMD list.
  // ----------------------------------------------------------------------
  testBegin("T07: prSupportsPostScript(CMD with POSTSCRIPT) → true");
  {
    bool r = prSupportsPostScript("MFG:Acme;MDL:X;CMD:POSTSCRIPT,PDF;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T08 — "PS3" in the COMMAND SET field (the alternate field name
  //  some manufacturers use).  Confirms the function checks both
  //  CMD and COMMAND SET, not just CMD.
  // ----------------------------------------------------------------------
  testBegin("T08: prSupportsPostScript(COMMAND SET with PS3) → true");
  {
    bool r = prSupportsPostScript("MFG:HP;MDL:LJ;COMMAND SET:PCL,PS3;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T09 — PCL-only device, no PostScript anywhere.  Must return
  //  false (both helper calls return 0, OR-of-zeros is 0).
  // ----------------------------------------------------------------------
  testBegin("T09: prSupportsPostScript(PCL-only printer) → false");
  {
    bool r = prSupportsPostScript("MFG:HP;MDL:DJ1000;CMD:PCL,PCL5;");
    testEnd(r == false);
  }

  // ----------------------------------------------------------------------
  //  T10 — NULL input.  Both prRegExMatchDevIDField() calls early-
  //  out negative; helper returns false because negatives are not
  //  treated as "matches".
  // ----------------------------------------------------------------------
  testBegin("T10: prSupportsPostScript(NULL) → false");
  {
    bool r = prSupportsPostScript(NULL);
    testEnd(r == false);
  }


  // ========================================================================
  //  GROUP 3 — prSupportsPDF
  //  --------------------------------------------------------------------
  //  Regex: "^(PDF)" (POSIX-extended, case-insensitive).
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T11 — Positive: PDF in CMD.
  // ----------------------------------------------------------------------
  testBegin("T11: prSupportsPDF(CMD with PDF) → true");
  {
    bool r = prSupportsPDF("MFG:HP;MDL:LJ;CMD:PCL,PS,PDF;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T12 — Case-insensitive: lowercase "pdf" still matches because
  //  REG_ICASE is set when the regex is compiled
  //  (pappl-retrofit.c:338).
  // ----------------------------------------------------------------------
  testBegin("T12: prSupportsPDF(lowercase 'pdf') → true (case-insensitive)");
  {
    bool r = prSupportsPDF("MFG:HP;MDL:LJ;CMD:pcl,pdf;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T13 — Negative: device that only speaks PostScript.
  // ----------------------------------------------------------------------
  testBegin("T13: prSupportsPDF(PostScript-only printer) → false");
  {
    bool r = prSupportsPDF("MFG:HP;MDL:LJ;CMD:POSTSCRIPT;");
    testEnd(r == false);
  }


  // ========================================================================
  //  GROUP 4 — prSupportsPCL5 / prSupportsPCL5c
  //  --------------------------------------------------------------------
  //  PCL 5  regex: "^(PCL([ -]?5([ -]?[ce])?)?)$"
  //  PCL 5c regex: "^(PCL[ -]?5[ -]?c)$"
  //
  //  The PCL5 regex is permissive — it matches "PCL", "PCL5",
  //  "PCL 5", "PCL-5", "PCL5e", "PCL5c", etc.  The PCL5c regex
  //  is strict — it only matches "PCL5c" / "PCL 5 c" / "PCL-5-c".
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T14 — Plain "PCL" satisfies the PCL5 regex (the inner group
  //  is optional via the trailing '?').
  // ----------------------------------------------------------------------
  testBegin("T14: prSupportsPCL5(CMD with bare 'PCL') → true");
  {
    bool r = prSupportsPCL5("MFG:HP;MDL:DJ;CMD:PCL;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T15 — "PCL5c" satisfies PCL5 (colour is just an option, a colour
  //  printer still speaks PCL 5).
  // ----------------------------------------------------------------------
  testBegin("T15: prSupportsPCL5(CMD with 'PCL5c') → true");
  {
    bool r = prSupportsPCL5("MFG:HP;MDL:CLJ;CMD:PCL5c,PJL;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T16 — "PCL5c" satisfies the strict PCL5c regex (the colour-
  //  capable detector).
  // ----------------------------------------------------------------------
  testBegin("T16: prSupportsPCL5c(CMD with 'PCL5c') → true");
  {
    bool r = prSupportsPCL5c("MFG:HP;MDL:CLJ;CMD:PCL5c,PJL;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T17 — Monochrome "PCL5" must NOT trigger the colour-only
  //  detector.  Confirms the strict regex correctly requires a
  //  trailing 'c'.
  // ----------------------------------------------------------------------
  testBegin("T17: prSupportsPCL5c(CMD with 'PCL5' mono) → false");
  {
    bool r = prSupportsPCL5c("MFG:HP;MDL:LJ;CMD:PCL5;");
    testEnd(r == false);
  }


  // ========================================================================
  //  GROUP 5 — prSupportsPCLXL + cross-PDL negatives
  //  --------------------------------------------------------------------
  //  PCL-XL regex: "^(PCL[ -]?XL|PXL|PCL[ -]?6)$"
  //  All three spellings ("PCLXL", "PXL", "PCL6") must hit.
  // ========================================================================

  // ----------------------------------------------------------------------
  //  T18 — "PCLXL" hits the first alternative.
  // ----------------------------------------------------------------------
  testBegin("T18: prSupportsPCLXL(CMD with 'PCLXL') → true");
  {
    bool r = prSupportsPCLXL("MFG:HP;MDL:LJ;CMD:PCLXL;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T19 — "PXL" hits the short-form alternative used by some HP
  //  drivers.
  // ----------------------------------------------------------------------
  testBegin("T19: prSupportsPCLXL(CMD with 'PXL') → true");
  {
    bool r = prSupportsPCLXL("MFG:HP;MDL:LJ;CMD:PXL,PJL;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T20 — "PCL6" hits the historical-name alternative (PCL-XL was
  //  originally marketed as PCL 6).
  // ----------------------------------------------------------------------
  testBegin("T20: prSupportsPCLXL(CMD with 'PCL6') → true");
  {
    bool r = prSupportsPCLXL("MFG:HP;MDL:LJ;CMD:PCL6;");
    testEnd(r == true);
  }

  // ----------------------------------------------------------------------
  //  T21 — Cross-PDL negative: a PCL5-only device must NOT be
  //  reported as supporting PCL-XL.  Confirms the regex anchors
  //  ('^' and '$') prevent "PCL5" from being mistaken for "PCL"
  //  inside the PXL alternative.
  // ----------------------------------------------------------------------
  testBegin("T21: prSupportsPCLXL(CMD with only 'PCL5') → false");
  {
    bool r = prSupportsPCLXL("MFG:HP;MDL:DJ;CMD:PCL5;");
    testEnd(r == false);
  }


  // ========================================================================
  //  Suite epilogue.
  // ========================================================================
  return (testsPassed ? 0 : 1);
}
