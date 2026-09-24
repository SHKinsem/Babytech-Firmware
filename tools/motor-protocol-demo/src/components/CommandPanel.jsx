import { certaintyLabel, formatBytes, formatCanId, hexByte } from '../protocol.js';
import { displayError, displayRange, displayUnit, displayValue, protocolValue } from '../field-display.js';
import { useEffect, useRef, useState } from 'react';
import { FrameList } from './FrameList.jsx';
import { CompactPanel } from './CompactPanel.jsx';
import { GlyphInfo, HelpTip } from './glyphs.jsx';

function Segmented({ label, options, value, onChange, name }) {
  // Groups with many options (homing 0..5, motion mode 0..2) get a modifier so
  // the device stylesheet can wrap them instead of overflowing the column.
  const many = options.length > 3;
  return (
    <div className={`segmented${many ? ' segmented--many' : ''}`} role="radiogroup" aria-label={label}>
      {options.map((option) => {
        // Variant options carry string keys while protocol fields carry numbers.
        const selected = typeof option.value === 'number'
          ? Number(value) === option.value
          : String(value ?? '') === String(option.value);
        return (
          <button
            type="button"
            key={`${name}-${option.value}`}
            role="radio"
            aria-checked={selected}
            className={`segmented__item${selected ? ' is-selected' : ''}`}
            onClick={() => onChange(option.value)}
          >
            {option.label}
          </button>
        );
      })}
    </div>
  );
}

function FieldRow({ field, value, error, onChange, inputId }) {
  const helpText = field.hint ? certaintyLabel(field.certainty) + '：' + field.hint : null;
  const [draft, setDraft] = useState(null);
  const lastWritten = useRef(null);
  useEffect(() => {
    if (lastWritten.current !== null && String(value ?? '') === String(lastWritten.current)) {
      lastWritten.current = null;
      return;
    }
    setDraft(null);
  }, [value, field.key, field.unit]);
  const shownValue = draft ?? displayValue(field, value);
  const shownError = displayError(field, shownValue, error);

  function edit(text) {
    const raw = protocolValue(field, text);
    lastWritten.current = raw;
    setDraft(text);
    onChange(raw);
  }

  return (
    <div className={'field' + (shownError ? ' has-error' : '')}>
      <div className="field__label">
        <label htmlFor={inputId}>
          {field.label}
          {field.unit ? <span className="field__unit">（{displayUnit(field)}）</span> : null}
        </label>
        {helpText ? <HelpTip label={field.label} text={helpText} /> : null}
      </div>
      <div className="field__control">
        {field.type === 'enum' ? (
          <Segmented
            name={field.key}
            label={field.label}
            options={field.options}
            value={value}
            onChange={onChange}
          />
        ) : (
          <div className="field__input">
            <input
              id={inputId}
              className={'input input--mono' + (field.control === 'hex' ? ' input--hex' : '')}
              inputMode={field.control === 'hex' ? 'text' : field.unit === '0.1 RPM' || field.unit === '0.1°' ? 'decimal' : 'numeric'}
              autoComplete="off"
              spellCheck={false}
              value={shownValue}
              aria-invalid={shownError ? 'true' : undefined}
              aria-describedby={shownError ? inputId + '-error' : undefined}
              onChange={(event) => edit(event.target.value)}
              onBlur={() => setDraft(null)}
            />
            <span className="field__range">
              {field.control === 'hex' ? '十六进制字节' : displayRange(field)}
            </span>
          </div>
        )}
        {shownError ? (
          <p className="field__error" id={inputId + '-error'} role="alert">
            {shownError}
          </p>
        ) : field.certainty === 'protocol' ? (
          <p className="field__meta">
            <span className="tag tag--protocol">协议字段：含义未在源码中定义</span>
          </p>
        ) : null}
      </div>
    </div>
  );
}

export function CommandPanel({
  device = false,
  item,
  variantKey,
  onVariantChange,
  values,
  onValueChange,
  editorMode,
  onEditorModeChange,
  rawText,
  onRawTextChange,
  onRawTextReplace,
  rawDirty,
  onResetRaw,
  identifiedItem,
  model,
  frames,
  annotations,
  address,
  addressError,
  gateReason,
  response,
  requestNotice = '',
  headingRef,
  onChooseCommand,
  onSend,
  onCopy,
}) {
  const variant = item.variants.find((entry) => entry.key === variantKey) ?? item.variants[0];
  const fields = variant.layout.filter((segment) => segment.kind === 'field');
  const missingFields = fields.filter((field) => model.errors?.[field.key]);
  const rawErrors = model.errors?._raw ?? [];
  const canSend = !gateReason;
  const baseInterface = !Number(values.sync ?? 0) && (
    ['enable', 'stop'].includes(item.id) ||
    (item.id === 'position' && variant.opcode === 0xcd && Number(values.motionMode) === 2)
  );

  const logicalHex = model.bytes?.length ? formatBytes(model.bytes) : '—';

  return (
    <section className="panel panel--command" aria-label="指令配置">
      <div className="command__head">
        <h2 className="panel__title" ref={headingRef} tabIndex={headingRef ? -1 : undefined}>{item.name}</h2>
        <span className="chip chip--code">0x{hexByte(variant.opcode)}</span>
        <span className={`chip ${baseInterface ? 'chip--ok' : 'chip--muted'}`}>
          {device ? '板端校验' : item.custom ? '自定义' : baseInterface ? '基础接口' : '驱动已实现'}
        </span>
        <span className="chip chip--soft">{item.groupName}</span>
        {onChooseCommand ? <button type="button" className="command__change-command" onClick={onChooseCommand}>更换指令</button> : null}
      </div>
      <p className="panel__desc">{item.summary}</p>

      <div className="subtabs" role="tablist" aria-label="编辑方式">
        <button
          type="button"
          role="tab"
          aria-selected={editorMode === 'form'}
          className={`subtabs__item${editorMode === 'form' ? ' is-active' : ''}`}
          onClick={() => onEditorModeChange('form')}
        >
          参数编辑
        </button>
        <button
          type="button"
          role="tab"
          aria-selected={editorMode === 'raw'}
          className={`subtabs__item${editorMode === 'raw' ? ' is-active' : ''}`}
          onClick={() => onEditorModeChange('raw')}
        >
          原始 HEX
        </button>
      </div>

      {editorMode === 'form' ? (
        <div className="form form--parameters">
          <div className="field">
            <span className="field__label">目标地址</span>
            <div className="field__control">
              <div className="field__input">
                <span className={`readonly-box${addressError ? ' has-error' : ''}`}>
                  {addressError ? '--' : hexByte(address)}
                </span>
                <span className="field__range">来自顶部 CAN ID 设置（只读，十进制 {address}）</span>
              </div>
            </div>
          </div>

          {item.variants.length > 1 ? (
            <div className="field">
              <span className="field__label">指令变体</span>
              <div className="field__control">
                <Segmented
                  name="variant"
                  label="指令变体"
                  options={item.variants.map((entry) => ({ value: entry.key, label: entry.label }))}
                  value={variant.key}
                  onChange={(key) => onVariantChange(String(key))}
                />
              </div>
            </div>
          ) : null}

          {item.custom ? (
            <>
              <FieldRow
                field={{
                  key: 'opcode',
                  label: '功能码',
                  type: 'int',
                  control: 'hex',
                  bytes: 1,
                  min: 0,
                  max: 255,
                  certainty: 'protocol',
                  hint: '1 字节十六进制，例如 A1。未知功能码只发送 TX 帧，不做模拟。',
                }}
                inputId="custom-opcode"
                value={values.opcode ?? ''}
                error={model.errors?.opcode}
                onChange={(value) => onValueChange('opcode', value)}
              />
              <FieldRow
                field={{
                  key: 'params',
                  label: '参数字节',
                  type: 'int',
                  control: 'hex',
                  bytes: 1,
                  min: 0,
                  max: 255,
                  certainty: 'protocol',
                  hint: '任意长度的十六进制字节串，可留空；末尾的 6B 由工具自动补齐。',
                }}
                inputId="custom-params"
                value={values.params ?? ''}
                error={model.errors?.params}
                onChange={(value) => onValueChange('params', value)}
              />
            </>
          ) : null}

          {fields.map((field) => (
            <FieldRow
              key={[item.id, variant.key, field.key].join('-')}
              field={field}
              inputId={`field-${field.key}`}
              value={values[field.key]}
              error={model.errors?.[field.key]}
              onChange={(value) => onValueChange(field.key, value)}
            />
          ))}

          {item.note ? (
            <p className="info-line">
              <GlyphInfo />
              <span>{item.note}</span>
            </p>
          ) : null}

          {missingFields.length > 0 ? (
            <p className="form__status form__status--error" role="alert">
              还有 {missingFields.length} 个参数未通过校验，修正后才能发送。
            </p>
          ) : null}

          {rawErrors.map((message) => (
            <p className="form__status form__status--error" key={message} role="alert">
              {message}
            </p>
          ))}
        </div>
      ) : (
        <div className="form">
          <div className="raw">
            <label className="field__label" htmlFor="raw-command">
              逻辑指令（地址 + 功能码 + 参数 + 6B）
            </label>
            <textarea
              id="raw-command"
              className={`raw__input${rawErrors.length ? ' has-error' : ''}`}
              value={rawText}
              spellCheck={false}
              rows={3}
              aria-invalid={rawErrors.length ? 'true' : undefined}
              onChange={(event) => onRawTextChange(event.target.value)}
            />
            <div className="raw__foot">
              <span className="raw__meta">
                {rawDirty
                  ? identifiedItem
                    ? `已匹配到「${identifiedItem.name}」：按匹配指令校验。${
                      model.bytes[0] === address
                        ? ''
                        : `（地址字节为 0x${hexByte(model.bytes[0])}，与顶部 CAN ID 设置不同）`
                    }`
                    : '未匹配到已知功能码；实机模式禁止发送。'
                  : '未修改：与左侧参数表单保持一致。'}
              </span>
              <span className="raw__actions">
                <button
                  type="button"
                  className="link-button"
                  onClick={() => onRawTextReplace((model.bytes ?? []).map(hexByte).join(' '))}
                  disabled={!model.bytes?.length}
                >
                  规整格式
                </button>
                <button type="button" className="link-button" onClick={onResetRaw} disabled={!rawDirty}>
                  恢复默认
                </button>
              </span>
            </div>
            {rawErrors.length > 0 ? (
              <ul className="raw__errors" role="alert">
                {rawErrors.map((message) => (
                  <li key={message}>{message}</li>
                ))}
              </ul>
            ) : null}
          </div>
        </div>
      )}

      <CompactPanel title="发送预览" desktopInitiallyOpen={false} className="compact-panel--command-preview"><div className="preview">
        <h3 className="section__title">发送预览</h3>

        <div className="preview__row">
          <span className="preview__label">逻辑指令</span>
          <div className="preview__body">
            <code className="code-box">{logicalHex}</code>
            <button
              type="button"
              className="link-button"
              onClick={() => onCopy(logicalHex, '逻辑指令')}
              disabled={!model.bytes?.length}
            >
              复制
            </button>
          </div>
        </div>

        <div className="preview__row">
          <span className="preview__label">实际 CAN 帧</span>
          <div className="preview__body preview__body--frames">
            <FrameList frames={frames} annotations={annotations} onCopy={onCopy} />
          </div>
        </div>
      </div></CompactPanel>

      <div className="actions">
        <button type="button" className="button button--primary" onClick={onSend} disabled={!canSend}>
          发送指令
        </button>
        <button
          type="button"
          className="button button--outline"
          onClick={() => onCopy(
            frames
              .map((frame) => `ID ${formatCanId(frame.canId)} DLC ${frame.dlc} DATA ${formatBytes(frame.data)}`)
              .join('\n'),
            'CAN 帧',
          )}
          disabled={frames.length === 0}
        >
          复制帧
        </button>
        {gateReason ? (
          <p className="actions__gate" role="status">
            <GlyphInfo />
            <span>{gateReason}</span>
          </p>
        ) : (
          <p className="actions__hint">
            <GlyphInfo />
            <span>指令提交后等待电机应答；收到应答不等于机械动作完成。</span>
          </p>
        )}
        {device && response ? <div className="command-result" role="status" aria-label="本次指令反馈">
          <div className="command-result__head"><strong>最近发送 · 电机 {response.address} · 0x{hexByte(response.opcode)}</strong>
            <span>{response.phase === 'sending' ? '正在提交' : response.phase === 'queued' ? '请求已入队' : response.phase === 'unknown' ? '请求结果未知' : response.phase === 'restarted' ? '板端已重启' : '板端拒绝'}</span></div>
          {response.phase === 'rejected' ? <p>板端拒绝：{response.detail}</p> : response.phase === 'restarted' ? <p>{response.detail}</p> : <>
            {response.phase === 'unknown' && response.detail ? <p>{response.detail}</p> : null}
            <p>{response.txSeen ? 'CAN TX 已在板端记录' : '尚未在板端记录看到 CAN TX'} · {response.rxCount ? `收到 ${response.rxCount} 包同地址/功能码 RX` : '尚未收到同地址/功能码 RX'}</p>
            {response.reply?.decoded ? <><strong>{response.reply.decoded.title}</strong><pre>{response.reply.decoded.text}</pre></>
              : response.reply ? <p>已收到原始回包：{formatBytes(response.reply.data)}{[0x42,0x43].includes(response.opcode) ? '（多包参数等待完整读回）' : '（布局未确认，保留原始字节）'}</p>
              : <p>等待总线回包；部分命令可能不返回应答。</p>}
            <small>按接收顺序关联，协议没有事务序号；同功能码的自动查询或其他发送也可能出现于此。</small>
          </>}
        </div> : null}
        {device && requestNotice && !response ? <p className="command__request-notice" role="status">请求结果：{requestNotice}</p> : null}
      </div>

      <p className="panel__foot">
        参数标注：<span className="tag tag--grounded">源码明确</span>
        <span className="tag tag--name">参数名推断</span>
        <span className="tag tag--protocol">协议字段</span>
        含义或单位未在源码中定义的字段按原始字节透传，界面不做猜测。
      </p>
    </section>
  );
}
