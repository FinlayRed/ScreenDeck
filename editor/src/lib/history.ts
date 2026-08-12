// SPDX-License-Identifier: GPL-3.0-or-later

export interface HistoryState<T> {
  undo: T[];
  redo: T[];
  current: T;
  /** Snapshot function used for every retained copy. Defaults to a deep
   * clone; callers may pass a structural snapshot that shares immutable
   * media blobs so undo history stays cheap (E4). */
  clone: (value: T) => T;
}

export const HISTORY_LIMIT = 50;

export function createHistory<T>(value: T, clone: (value: T) => T = structuredClone): HistoryState<T> {
  return { undo: [], redo: [], current: clone(value), clone };
}

export function recordHistory<T>(history: HistoryState<T>, value: T): HistoryState<T> {
  return {
    undo: [...history.undo, history.clone(history.current)].slice(-HISTORY_LIMIT),
    redo: [],
    current: history.clone(value),
    clone: history.clone,
  };
}

/** Replaces the most recent history entry instead of adding one, so a burst
 * of rapid text edits collapses into a single undo step back to the state
 * before the burst began. */
export function coalesceHistory<T>(history: HistoryState<T>, value: T): HistoryState<T> {
  return {
    undo: history.undo.length === 0 ? [history.current] : history.undo,
    redo: [],
    current: history.clone(value),
    clone: history.clone,
  };
}

export function undoHistory<T>(history: HistoryState<T>): HistoryState<T> | null {
  const previous = history.undo.at(-1);
  if (!previous) return null;
  return {
    undo: history.undo.slice(0, -1),
    redo: [history.clone(history.current), ...history.redo].slice(0, HISTORY_LIMIT),
    current: history.clone(previous),
    clone: history.clone,
  };
}

export function redoHistory<T>(history: HistoryState<T>): HistoryState<T> | null {
  const next = history.redo[0];
  if (!next) return null;
  return {
    undo: [...history.undo, history.clone(history.current)].slice(-HISTORY_LIMIT),
    redo: history.redo.slice(1),
    current: history.clone(next),
    clone: history.clone,
  };
}
