// Local drafts for the device page's editable inputs.
//
// Two things on the device page are typed by hand and must survive navigation,
// a tab change and a reload:
//   * the command laboratory's form / HEX draft, kept per motor, per command and
//     per variant;
//   * the 常规试动 trial fields, kept per motor.
//
// Everything stored here is a LOCAL DRAFT. It is never a board-confirmed value,
// it is never sent automatically, and no board state is kept: no passwords, no
// enable/online/limits/acknowledgement fields and no execution state. The queued
// program owns its own storage and is untouched by this module.
//
// Pure module: no React, no timers, no network. Storage access is injected so
// the sanitizer is unit testable, and every read or write is defensive - a
// corrupt, foreign, oversized or unreadable payload degrades to safe defaults
// instead of throwing at the page.
//
// Stored shape (version 1):
//   {
//     version: 1,
//     seq: 12,                       // last used sequence number
//     lastMotorId: "2", lastCommandId: "position", lastVariantKey: "limit",
//     forms:   { "<motor>|<command>|<variant>": {values?, mode, raw, dirty, used} },
//     manuals: { "<motor>": {dir, angle, speed, accel, decel, current} },
//   }

export const DRAFTS_VERSION = 1;
export const DRAFTS_KEY = 'babytech.device-drafts';
// Bounds. They keep a hostile or merely broken payload from filling the origin
// storage and from making every restore expensive.
export const FORM_LIMIT = 40;
export const MANUAL_LIMIT = 16;
export const FIELD_LIMIT = 24;
export const VALUE_TEXT_MAX = 64;
export const RAW_TEXT_MAX = 512;
export const MODES = ['form', 'raw'];

// Only the primitives the forms actually hold. Everything else (objects,
// arrays, functions, null/undefined) is dropped rather than coerced.
// An empty string is a real draft: it is what a cleared input holds, and it must
// come back empty instead of being replaced by a default.
function sanitizeValue(value) {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return Number.isFinite(value) ? value : undefined;
  if (typeof value === 'string') return value.slice(0, VALUE_TEXT_MAX);
  return undefined;
}

/** Drops unknown/invalid entries. `keys` limits the result to a known layout. */
export function sanitizeFields(raw, keys = null) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const allowed = Array.isArray(keys) ? new Set(keys) : null;
  const out = {};
  let count = 0;
  for (const key of Object.keys(raw)) {
    if (allowed && !allowed.has(key)) continue;
    if (typeof key !== 'string' || !key.length || key.length > 64) continue;
    const value = sanitizeValue(raw[key]);
    if (value === undefined) continue;
    out[key] = value;
    if (++count >= FIELD_LIMIT) break;
  }
  return Object.keys(out).length ? out : null;
}

/** The stored subset for a known layout: defaults fill whatever is missing. */
export function pickFields(values, keys) {
  const clean = sanitizeFields(values, keys);
  return clean ?? {};
}

function sanitizeRaw(raw) {
  return typeof raw === 'string' ? raw.slice(0, RAW_TEXT_MAX) : '';
}

// Trial fields are kept as values plus their own timestamp, so the values map
// stays exactly the set of editable keys.
function sanitizeManual(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const values = sanitizeFields(raw.values);
  if (!values) return null;
  return {values, used: Number.isFinite(Number(raw.used)) ? Number(raw.used) : 0};
}

function sanitizeForm(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const form = {
    values: sanitizeFields(raw.values),
    mode: MODES.includes(raw.mode) ? raw.mode : 'form',
    raw: sanitizeRaw(raw.raw),
    dirty: raw.dirty === true,
    used: Number.isFinite(Number(raw.used)) ? Number(raw.used) : 0,
  };
  // A selection entry may carry no values yet (the variant was picked but never
  // edited): that is a valid draft meaning "defaults for this key".
  return form;
}

/**
 * Storage accessor that cannot throw. Private-mode or disabled storage returns
 * null, and the page then simply keeps its drafts in memory.
 */
export function safeStorage() {
  try {
    const storage = globalThis.localStorage;
    if (!storage) return null;
    // A read probe catches the browsers that expose the object but throw on use.
    const probe = '__babytech_probe__';
    storage.setItem(probe, '1');
    storage.removeItem(probe);
    return storage;
  } catch {
    return null;
  }
}

export function emptyDrafts() {
  return {
    version: DRAFTS_VERSION, seq: 0,
    lastMotorId: '', lastCommandId: '', lastVariantKey: '',
    forms: {}, manuals: {},
  };
}

/** Keeps the most recently used entries, newest first. */
function trimMap(map, limit) {
  const keys = Object.keys(map);
  if (keys.length <= limit) return map;
  keys.sort((a, b) => (map[b]?.used ?? 0) - (map[a]?.used ?? 0));
  const kept = {};
  for (const key of keys.slice(0, limit)) kept[key] = map[key];
  return kept;
}

/**
 * Reads and sanitizes the stored drafts. Any failure - missing storage, foreign
 * JSON, another version, wrong types - yields empty drafts, never an exception.
 */
export function readDrafts(storage) {
  const empty = emptyDrafts();
  if (!storage) return empty;
  try {
    const text = storage.getItem(DRAFTS_KEY);
    if (!text) return empty;
    const raw = JSON.parse(text);
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return empty;
    if (raw.version !== DRAFTS_VERSION) return empty;
    const drafts = emptyDrafts();
    drafts.seq = Number.isFinite(Number(raw.seq)) ? Math.max(0, Math.trunc(Number(raw.seq))) : 0;
    const motorId = sanitizeValue(raw.lastMotorId);
    const commandId = sanitizeValue(raw.lastCommandId);
    const variantKey = sanitizeValue(raw.lastVariantKey);
    if (typeof motorId === 'string') drafts.lastMotorId = motorId;
    if (typeof commandId === 'string') drafts.lastCommandId = commandId;
    if (typeof variantKey === 'string') drafts.lastVariantKey = variantKey;
    if (raw.forms && typeof raw.forms === 'object' && !Array.isArray(raw.forms)) {
      for (const key of Object.keys(raw.forms)) {
        if (!key.length || key.length > 128) continue;
        const form = sanitizeForm(raw.forms[key]);
        if (form) drafts.forms[key] = form;
      }
      drafts.forms = trimMap(drafts.forms, FORM_LIMIT);
    }
    if (raw.manuals && typeof raw.manuals === 'object' && !Array.isArray(raw.manuals)) {
      for (const key of Object.keys(raw.manuals)) {
        if (!key.length || key.length > 32) continue;
        const entry = sanitizeManual(raw.manuals[key]);
        if (entry) drafts.manuals[key] = entry;
      }
      drafts.manuals = trimMap(drafts.manuals, MANUAL_LIMIT);
    }
    return drafts;
  } catch {
    return empty;
  }
}

/** Best-effort write; a full or blocked storage never breaks the page. */
export function writeDrafts(storage, drafts) {
  if (!storage) return false;
  try {
    storage.setItem(DRAFTS_KEY, JSON.stringify(drafts));
    return true;
  } catch {
    return false;
  }
}

export function formKey(motorId, commandId, variantKey) {
  return `${motorId}|${commandId}|${variantKey}`;
}

/** The stored draft for one key, or null when this form was never touched. */
export function getForm(drafts, motorId, commandId, variantKey) {
  if (!drafts?.forms) return null;
  const form = drafts.forms[formKey(motorId, commandId, variantKey)];
  if (!form) return null;
  return {
    values: form.values ? { ...form.values } : null,
    mode: form.mode,
    raw: form.raw,
    dirty: form.dirty,
  };
}

/**
 * Returns a NEW drafts object with `patch` merged into one form entry and the
 * entry marked as the most recently used one. Commands, variants and motor ids
 * are opaque strings here: an unknown key is stored like any other, and the page
 * decides what it can restore.
 */
export function putForm(drafts, motorId, commandId, variantKey, patch) {
  const base = drafts ?? emptyDrafts();
  const key = formKey(motorId, commandId, variantKey);
  const seq = (base.seq ?? 0) + 1;
  const previous = base.forms?.[key] ?? {};
  const next = {
    values: patch.values !== undefined ? (sanitizeFields(patch.values) ?? {}) : previous.values,
    mode: MODES.includes(patch.mode) ? patch.mode : (previous.mode ?? 'form'),
    raw: patch.raw !== undefined ? sanitizeRaw(patch.raw) : (previous.raw ?? ''),
    dirty: patch.dirty !== undefined ? patch.dirty === true : !!previous.dirty,
    used: seq,
  };
  return {
    ...base,
    seq,
    forms: trimMap({ ...(base.forms ?? {}), [key]: next }, FORM_LIMIT),
  };
}

/**
 * The variant most recently used for one motor+command, or null. This is what
 * lets the page come back to the variant the user was editing.
 */
export function lastVariantFor(drafts, motorId, commandId) {
  const prefix = `${motorId}|${commandId}|`;
  let best = null, bestUsed = -1;
  for (const key of Object.keys(drafts?.forms ?? {})) {
    if (!key.startsWith(prefix)) continue;
    const used = drafts.forms[key]?.used ?? 0;
    if (used > bestUsed) { bestUsed = used; best = key.slice(prefix.length); }
  }
  return best;
}

/** The stored trial-field draft of one motor, or null when it was never edited. */
export function getManual(drafts, motorId) {
  const entry = drafts?.manuals?.[String(motorId)];
  return entry?.values ? { ...entry.values } : null;
}

export function putManual(drafts, motorId, values) {
  const base = drafts ?? emptyDrafts();
  const clean = sanitizeFields(values);
  if (!clean) return base;
  const seq = (base.seq ?? 0) + 1;
  return {
    ...base,
    seq,
    manuals: trimMap(
      { ...(base.manuals ?? {}), [String(motorId)]: { values: clean, used: seq } },
      MANUAL_LIMIT,
    ),
  };
}

/** Remembers the last valid selection so a reload can come back to it. */
export function putSelection(drafts, { motorId, commandId, variantKey }) {
  const base = drafts ?? emptyDrafts();
  return {
    ...base,
    lastMotorId: typeof motorId === 'string' ? motorId.slice(0, 32) : base.lastMotorId,
    lastCommandId: typeof commandId === 'string' ? commandId.slice(0, 64) : base.lastCommandId,
    lastVariantKey: typeof variantKey === 'string' ? variantKey.slice(0, 64) : base.lastVariantKey,
  };
}
