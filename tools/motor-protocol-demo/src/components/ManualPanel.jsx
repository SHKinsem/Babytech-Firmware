import { formatBytes, hexByte, MANUAL_LIMITS } from '../protocol.js';
import { formatPosition } from '../simulation.js';
import { FrameList } from './FrameList.jsx';
import { GlyphInfo, HelpTip } from './glyphs.jsx';

function MoveField({ id, label, unit, value, error, help, range, onChange }) {
  return (
    <div className={`field${error ? ' has-error' : ''}`}>
      <div className="field__label">
        <label htmlFor={id}>
          {label}
          {unit ? <span className="field__unit">（{unit}）</span> : null}
        </label>
        {help ? <HelpTip label={label} text={help} /> : null}
      </div>
      <div className="field__control">
        <div className="field__input">
          <input
            id={id}
            className="input input--mono"
            inputMode="decimal"
            autoComplete="off"
            value={value}
            aria-invalid={error ? 'true' : undefined}
            aria-describedby={error ? `${id}-error` : undefined}
            onChange={(event) => onChange(event.target.value)}
          />
          <span className="field__range">{range}</span>
        </div>
        {error ? (
          <p className="field__error" id={`${id}-error`} role="alert">
            {error}
          </p>
        ) : null}
      </div>
    </div>
  );
}

export function ManualPanel({
  device = false,
  values,
  errors,
  onChange,
  motor,
  address,
  prediction,
  bytes,
  frames,
  annotations,
  gateReason,
  onSend,
  onStop,
  onCopy,
  onGoToEnable,
}) {
  const targetTenths = motor.positionTenths == null ? null : motor.positionTenths + (prediction.ok ? prediction.deltaTenths : 0);

  return (
    <div className="manual">
      <section className="manual__form" aria-label="相对运动参数">
        <div className="command__head">
          <h2 className="panel__title">常规试动</h2>
          <span className="chip chip--code">0x{hexByte(0xcd)}</span>
          <span className="chip chip--ok">已接入</span>
          <span className="chip chip--soft">梯形位置 + 限流</span>
        </div>
        <p className="panel__desc">
          {device ? '相对位置运动：真实使能应答和新鲜静止反馈到达后才能执行。' : '相对位置运动，与指令实验室共用 CD 组帧和模拟状态。'}
        </p>

        <div className="form">
          <MoveField
            id="manual-angle"
            label="相对角度"
            unit="°"
            value={values.angle}
            error={errors.angle}
            help="正数幅值，内部按 0.1° 取整后作为 CD 指令的行程字；转向由「方向」决定。"
            range={`${MANUAL_LIMITS.minAbsAngleDeg}..${MANUAL_LIMITS.maxAbsAngleDeg}°`}
            onChange={(value) => onChange('angle', value)}
          />

          <div className="field">
            <span className="field__label">
              <span>方向</span>
            </span>
            <div className="field__control">
              <div className="segmented" role="radiogroup" aria-label="方向">
                <button
                  type="button"
                  role="radio"
                  aria-checked={Number(values.dir) === 0}
                  className={`segmented__item${Number(values.dir) === 0 ? ' is-selected' : ''}`}
                  onClick={() => onChange('dir', 0)}
                >
                  正向
                </button>
                <button
                  type="button"
                  role="radio"
                  aria-checked={Number(values.dir) === 1}
                  className={`segmented__item${Number(values.dir) === 1 ? ' is-selected' : ''}`}
                  onClick={() => onChange('dir', 1)}
                >
                  反向
                </button>
              </div>
            </div>
          </div>

          <MoveField
            id="manual-speed"
            label="速度"
            unit="RPM"
            value={values.speed}
            error={errors.speed}
            help="按 0.1 RPM 下发（MotionCore kTenthsPerRpm）。"
            range={`${MANUAL_LIMITS.minSpeedRpm}..${MANUAL_LIMITS.maxSpeedRpm} RPM`}
            onChange={(value) => onChange('speed', value)}
          />
          <MoveField
            id="manual-accel"
            label="加速度"
            unit="RPM/s"
            value={values.accel}
            error={errors.accel}
            help="整数 RPM/s；位置指令的加减速在线上不乘 10。"
            range={`${MANUAL_LIMITS.minAccelRpmS}..${MANUAL_LIMITS.maxAccelRpmS} RPM/s`}
            onChange={(value) => onChange('accel', value)}
          />
          <MoveField
            id="manual-decel"
            label="减速度"
            unit="RPM/s"
            value={values.decel}
            error={errors.decel}
            help="整数 RPM/s。"
            range={`${MANUAL_LIMITS.minAccelRpmS}..${MANUAL_LIMITS.maxAccelRpmS} RPM/s`}
            onChange={(value) => onChange('decel', value)}
          />
          <MoveField
            id="manual-current"
            label="电流上限"
            unit="mA"
            value={values.current}
            error={errors.current}
            help="力矩由电流决定，界面不使用 Nm。"
            range={`${MANUAL_LIMITS.minCurrentMa}..${MANUAL_LIMITS.maxCurrentMa} mA`}
            onChange={(value) => onChange('current', value)}
          />

          <div className="field">
            <span className="field__label">
              <span>预测结果</span>
            </span>
            <div className="field__control">
              <div className="prediction">
                <span className="prediction__item">
                  目标 <strong>{prediction.ok && targetTenths != null ? formatPosition(targetTenths) : '—'}</strong>
                </span>
                <span className="prediction__item">
                  时长 <strong>{prediction.ok ? `${prediction.durationMs} ms` : '—'}</strong>
                </span>
                <span className="prediction__item">
                  行程 <strong>{prediction.ok ? `${prediction.plan.clk} × 0.1°` : '—'}</strong>
                </span>
                <span className="prediction__item">
                  方向 <strong>{Number(values.dir) === 1 ? '反向' : '正向'}</strong>
                </span>
              </div>
            </div>
          </div>

          <p className="info-line">
            <GlyphInfo />
            <span>
              运动需要电机处于使能状态；当前 {motor.enabled ? '已使能' : '未使能'}（地址 0x{hexByte(address)}）。
            </span>
          </p>
        </div>

        <div className="actions">
          <button type="button" className="button button--primary" onClick={onSend} disabled={Boolean(gateReason)}>
            发送运动指令
          </button>
          <button type="button" className="button button--outline" onClick={onStop}>
            立即停止
          </button>
          {!motor.enabled && onGoToEnable ? (
            <button type="button" className="link-button" onClick={onGoToEnable}>
              去指令实验室发送使能
            </button>
          ) : null}
          {gateReason ? (
            <p className="actions__gate" role="status">
              <GlyphInfo />
              <span>{gateReason}</span>
            </p>
          ) : (
            <p className="actions__hint">
              <GlyphInfo />
              <span>{device ? '停止请求独立发送，以新的静止反馈确认停止。' : '停止会作废未完成的模拟运动。'}</span>
            </p>
          )}
        </div>
      </section>

      <section className="manual__preview" aria-label="CD 帧预览">
        <h3 className="section__title">发送预览</h3>
        <div className="preview">
          <div className="preview__row">
            <span className="preview__label">逻辑指令</span>
            <div className="preview__body">
              <code className="code-box">{bytes.length ? formatBytes(bytes) : '—'}</code>
              <button
                type="button"
                className="link-button"
                disabled={bytes.length === 0}
                onClick={() => onCopy(formatBytes(bytes), '逻辑指令')}
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
        </div>

        <p className="panel__foot">
          角度、速度与电流的取值区间沿用 MotionCore.h 的请求校验范围；越界直接报错，不做静默截断。
        </p>
      </section>
    </div>
  );
}
