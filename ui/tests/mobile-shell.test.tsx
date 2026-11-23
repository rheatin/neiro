// The shell a phone and a foldable get: which one the measured width asks
// for, and the band that keeps the current one while a drag crosses 700 pt.

import { expect, test } from "nsh/test";
import { nextMobileShell } from "../src/components/layout";

test("a width at or above 700 pt unfolds, below 660 pt folds", () => {
  expect(nextMobileShell("folded", 700)).toBe("unfolded");
  expect(nextMobileShell("unfolded", 700)).toBe("unfolded");
  expect(nextMobileShell("unfolded", 659)).toBe("folded");
  expect(nextMobileShell("folded", 659)).toBe("folded");
  expect(nextMobileShell("folded", 1024)).toBe("unfolded");
});

test("between 660 and 700 pt the shell on show is kept", () => {
  for (const width of [660, 680, 699]) {
    expect(nextMobileShell("folded", width)).toBe("folded");
    expect(nextMobileShell("unfolded", width)).toBe("unfolded");
  }
});
