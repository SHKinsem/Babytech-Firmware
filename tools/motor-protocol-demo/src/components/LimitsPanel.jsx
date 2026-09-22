import { useEffect, useState } from 'react';
import { DEFAULT_LIMITS, LIMIT_FIELDS, checkLimitsDraft, limitsEqual, limitsToDraft } from '../device-limits.js';
import { GlyphInfo, HelpTip } from './glyphs.jsx';

const emptyDraft = () => Object.fromEntries(LIMIT_FIELDS.map((field) => [field.key, '']));

function LimitRow({ field, value, error, disabled, onChange }) {
  const inputId = `limit-${field.key}`;
  const range = `${field.min}..${field.max} ${field.unit}`;
  return (
    <div className={`field${error ? ' has-error' : ''}`}>
      <div className="field__label">
        <label htmlFor={inputId}>
          {field.label}
          <span className="field__unit">（{field.unit}）</span>
        </label>
        <HelpTip label={field.label} text={`板端可调策略；实现上限 ${range}，越界会被拒绝。${field.hint}`} />
      </div>
      <div className="field__control">
        <div className="field__input">
          <input
            id={inputId}
            className="input input--mono"
            inputMode={field.integer ? 'numeric' : 'decimal'}
            autoComplete="off"
            spellCheck={false}
            disabled={disabled}
            value={value}
            aria-invalid={error ? 'true' : undefined}
            aria-describedby={error ? `${inputId}-error` : undefined}
            onChange={(event) => onChange(field.key, event.target.value)}
          />
          <span className="field__range">实现上限 {range}</span>
        </div>
        {error ? (
          <p className="field__error" id={`${inputId}-error`} role="alert">
            {error}
          </p>
        ) : null}
      </div>
    </div>
  );
}

/**
 * 调试限制: the editable board policy.
 *
 * The confirmed limits live in DeviceApp (they gate motion everywhere). This
 * panel only owns the draft: it never posts on its own, never saves per
 * keystroke and never shows a value as saved before the board answered.
 */
export function LimitsPanel({
  limits,
  state,
  loadError,
  saving,
  saveError,
  saveNotice,
  connected,
  onSave,
  onReload,
  onDraftEdit = () => {},
}) {
  const [draft, setDraft] = useState(() => emptyDraft());
  const [dirty, setDirty] = useState(false);
  const ready = state === 'ready' && Boolean(limits);

  // A poll refresh must never overwrite what the user is editing. Before the
  // first successful read the fields stay empty: showing 120/240 there would
  // look like loaded board values.
  useEffect(() => {
    if (!dirty) setDraft(ready ? limitsToDraft(limits) : emptyDraft());
  }, [limits, ready, dirty]);

  const check = checkLimitsDraft(draft);
  // Field errors appear as soon as the draft is edited: the save button is
  // disabled while anything is out of range, so the reason must be visible.
  const errors = dirty ? check.errors : {};
  const changed = ready && !limitsEqual(check.limits, limits);
  const canSave = ready && connected && dirty && changed && check.ok && !saving;

  function edit(key, value) {
    setDraft((prev) => ({ ...prev, [key]: value }));
    setDirty(true);
    onDraftEdit();
  }

  async function save() {
    if (!check.ok) return;
    const ok = await onSave(check.limits);
    if (ok) setDirty(false);
  }

  function useDefaults() {
    setDraft(limitsToDraft(DEFAULT_LIMITS));
    setDirty(true);
    onDraftEdit();
  }

  function discard() {
    setDraft(ready ? limitsToDraft(limits) : emptyDraft());
    setDirty(false);
  }

  return (
    <div className="limits-page">
      <section className="limits-card" aria-label="板上当前限制">
        <div className="limits-card__head">
          <h2>板上当前限制</h2>
          <span className={`chip ${ready ? 'chip--ok' : state === 'loading' ? 'chip--soft' : 'chip--muted'}`}>
            {ready ? '已加载' : state === 'loading' ? '读取中' : '不可用'}
          </span>
        </div>
        {ready ? (
          <>
            <p className="limits-card__sub">
              板端全局策略（对所有电机地址生效），保存在 NVS，重启后仍然有效。
            </p>
            <dl className="limits-summary">
              {LIMIT_FIELDS.map((field) => (
                <div key={field.key}>
                  <dt>{field.label}</dt>
                  <dd>
                    {limits[field.key]} {field.unit}
                    {field.key === 'experimentSeconds' && Number(limits[field.key]) === 0 ? (
                      <em>持续运行，手动停止</em>
                    ) : null}
                  </dd>
                </div>
              ))}
            </dl>
            <p className="limits-card__note">
              试验自动停止：{Number(limits.experimentSeconds) === 0
                ? '已关闭定时停止，速度／力矩试验会持续运行，需要手动停止；反馈超时与故障保护仍然独立生效。'
                : `速度／力矩试验最多运行 ${limits.experimentSeconds} 秒后由板端自动停止。`}
            </p>
          </>
        ) : state === 'loading' ? (
          <p className="limits-card__note">正在读取 /api/limits，读取完成前运动指令保持禁用（读取、停止与失能不受影响）。</p>
        ) : (
          <>
            <p className="device-error" role="alert">{loadError || '未能读取板端限制。'}</p>
            <p className="limits-card__note">
              未取到限制时本页不会假定 120/240 等默认值：运动指令保持禁用，直到读取成功。
            </p>
            <button type="button" className="button button--outline" onClick={onReload}>重新读取</button>
          </>
        )}
      </section>

      <section className="limits-card" aria-label="编辑限制">
        <div className="limits-card__head">
          <h2>编辑限制</h2>
          {!ready ? null : dirty
            ? <span className="chip chip--soft">未保存草稿</span>
            : <span className="chip chip--muted">与板上一致</span>}
        </div>
        <p className="limits-card__sub">
          输入框是本地草稿：只有点击「保存到板上」才会写入板端 NVS。保存不会发送任何运动或使能指令。
        </p>

        <div className="form">
          {LIMIT_FIELDS.map((field) => (
            <LimitRow
              key={field.key}
              field={field}
              value={draft[field.key] ?? ''}
              error={errors[field.key]}
              disabled={!ready || saving}
              onChange={edit}
            />
          ))}
        </div>

        <div className="limits-actions">
          <button type="button" className="button button--primary" onClick={save} disabled={!canSave}>
            {saving ? '保存中…' : '保存到板上'}
          </button>
          <button type="button" className="button button--outline" onClick={discard} disabled={!dirty || saving}>
            放弃修改
          </button>
          <button type="button" className="link-button" onClick={useDefaults} disabled={!ready || saving}>
            填入默认值（仅本地）
          </button>
        </div>

        {!connected ? (
          <p className="limits-state limits-state--warn"><GlyphInfo /><span>设备未连接：无法保存，草稿会保留到重新连接。</span></p>
        ) : !ready ? (
          <p className="limits-state limits-state--warn"><GlyphInfo /><span>尚未确认板上限制，保存已禁用。</span></p>
        ) : dirty && !check.ok ? (
          <p className="limits-state limits-state--warn"><GlyphInfo /><span>有字段超出实现上限，修正后才能保存。</span></p>
        ) : dirty && !changed ? (
          <p className="limits-state"><GlyphInfo /><span>草稿与板上数值相同，无需保存。</span></p>
        ) : dirty ? (
          <p className="limits-state"><GlyphInfo /><span>草稿尚未写入板端；保存成功后以板端返回值为准。</span></p>
        ) : (
          <p className="limits-state"><GlyphInfo /><span>当前显示的是板端已保存的数值，轮询刷新不会覆盖正在编辑的草稿。</span></p>
        )}

        {saveError ? <p className="device-error" role="alert">{saveError}</p> : null}
        {saveNotice ? <p className="device-notice" role="status">{saveNotice}</p> : null}
      </section>
    </div>
  );
}
