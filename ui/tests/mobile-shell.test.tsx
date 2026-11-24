// The shell a phone, a foldable, and a tablet get: which one the measured
// width asks for, and the band that keeps the current one while a drag
// crosses 1000 pt.

import { expect, test } from "nsh/test";
import { nextMobileShell } from "../src/components/layout";

test("a width at or above 1000 pt unfolds, below 960 pt folds", () => {
  expect(nextMobileShell("folded", 1000)).toBe("unfolded");
  expect(nextMobileShell("unfolded", 1000)).toBe("unfolded");
  expect(nextMobileShell("unfolded", 959)).toBe("folded");
  expect(nextMobileShell("folded", 959)).toBe("folded");
  expect(nextMobileShell("folded", 1024)).toBe("unfolded");
  expect(nextMobileShell("folded", 834)).toBe("folded");
});

test("between 960 and 1000 pt the shell on show is kept", () => {
  for (const width of [960, 980, 999]) {
    expect(nextMobileShell("folded", width)).toBe("folded");
    expect(nextMobileShell("unfolded", width)).toBe("unfolded");
  }
});
