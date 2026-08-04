// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from "vitest";
import { createHistory, HISTORY_LIMIT, recordHistory, redoHistory, undoHistory } from "./history";

describe("project history", () => {
  it("undoes, redoes, and clears redo after a branch", () => {
    let history = createHistory({ value: 0 });
    history = recordHistory(history, { value: 1 });
    history = recordHistory(history, { value: 2 });
    history = undoHistory(history)!;
    expect(history.current.value).toBe(1);
    history = redoHistory(history)!;
    expect(history.current.value).toBe(2);
    history = undoHistory(history)!;
    history = recordHistory(history, { value: 9 });
    expect(history.redo).toHaveLength(0);
    expect(history.current.value).toBe(9);
  });

  it("bounds retained snapshots", () => {
    let history = createHistory({ value: 0 });
    for (let value = 1; value < HISTORY_LIMIT + 10; value += 1) {
      history = recordHistory(history, { value });
    }
    expect(history.undo).toHaveLength(HISTORY_LIMIT);
  });
});
