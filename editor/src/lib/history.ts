// SPDX-License-Identifier: GPL-3.0-or-later

export interface HistoryState<T> {
  undo: T[];
  redo: T[];
  current: T;
}

export const HISTORY_LIMIT = 50;

export function createHistory<T>(value: T): HistoryState<T> {
  return { undo: [], redo: [], current: structuredClone(value) };
}

export function recordHistory<T>(history: HistoryState<T>, value: T): HistoryState<T> {
  return {
    undo: [...history.undo, structuredClone(history.current)].slice(-HISTORY_LIMIT),
    redo: [],
    current: structuredClone(value),
  };
}

export function undoHistory<T>(history: HistoryState<T>): HistoryState<T> | null {
  const previous = history.undo.at(-1);
  if (!previous) return null;
  return {
    undo: history.undo.slice(0, -1),
    redo: [structuredClone(history.current), ...history.redo].slice(0, HISTORY_LIMIT),
    current: structuredClone(previous),
  };
}

export function redoHistory<T>(history: HistoryState<T>): HistoryState<T> | null {
  const next = history.redo[0];
  if (!next) return null;
  return {
    undo: [...history.undo, structuredClone(history.current)].slice(-HISTORY_LIMIT),
    redo: history.redo.slice(1),
    current: structuredClone(next),
  };
}
