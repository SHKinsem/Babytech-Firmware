import { formatBytes, formatCanId, hexByte } from '../protocol.js';
import { formatCurrent, formatPosition, formatVelocity } from '../simulation.js';
import { GlyphDot, GlyphInfo } from './glyphs.jsx';

const STATUS_TONE = {
  received: 'ok',
  feedback: 'ok',
  queued: 'info',
  unsupported: 'muted',
  idle: 'muted',
  error: 'warn',
};

const CAPABILITIES = [
  {
    tone: 'ok',
    title: '基础接口',
    detail: '使能、相对运动、停止',
  },
  {
    tone: 'ok',
    title: '驱动已实现',
    detail: '扩展指令、参数与组帧',
  },
  {
    tone: 'idle',
    title: '当前演示',
    detail: '全部反馈均为本地模拟',
  },
];

export function FeedbackPanel({ motor, address, pendingCount }) {
  const response = motor.lastResponse;
  const tone = STATUS_TONE[motor.status?.kind] ?? 'muted';

  return (
    <section className="panel panel--feedback" aria-label="电机反馈">
      <div className="feedback__head">
        <h2 className="panel__title">电机反馈</h2>
        <span className={`chip ${motor.enabled ? 'chip--ok' : 'chip--muted'}`}>
          {motor.enabled ? '已使能' : '未使能'}
        </span>
      </div>

      <p className="feedback__address">
        当前电机 <code className="code-inline">0x{hexByte(address)}</code> · 模拟反馈
      </p>

      <dl className="metrics">
        <div className="metrics__row">
          <dt>位置</dt>
          <dd>{formatPosition(motor.positionTenths)}</dd>
        </div>
        <div className="metrics__row">
          <dt>速度</dt>
          <dd>{formatVelocity(motor.velocityTenths)}</dd>
        </div>
        <div className="metrics__row">
          <dt>电流</dt>
          <dd>{formatCurrent(motor.currentMa)}</dd>
        </div>
      </dl>

      <p className="metrics__unit">
        模拟读数 · 位置 {motor.positionTenths} × 0.1°、速度 {motor.velocityTenths} × 0.1 RPM、电流 {Math.round(motor.currentMa)} mA（单位见 MotionCore.h）
      </p>

      {pendingCount > 0 ? (
        <p className="feedback__queue">
          <GlyphInfo />
          <span>待触发队列 {pendingCount} 条：发送 FF（同步触发）后按顺序模拟执行。</span>
        </p>
      ) : null}

      <div className="divider" />

      <h3 className="section__title">最近应答</h3>
      <div className="response">
        <code className={`response__data${response ? '' : ' is-empty'}`}>
          {response ? formatBytes(response.data) : '暂无应答'}
        </code>
        <span className={`chip chip--${tone}`}>{motor.status?.text ?? '暂无应答'}</span>
      </div>
      <p className="response__note">
        {response
          ? `模拟应答 · 帧 ID ${formatCanId(motor.lastResponse.canId ?? (address << 8))} · 不代表运动完成。`
          : '尚未发送指令；所有应答均为本地模拟，不会访问真实硬件。'}
      </p>

      <div className="divider" />

      <h3 className="section__title">能力接入</h3>
      <ul className="capabilities">
        {CAPABILITIES.map((entry) => (
          <li key={entry.title}>
            <GlyphDot tone={entry.tone} />
            <span className={`capabilities__title${entry.tone === 'ok' ? ' is-on' : ''}`}>{entry.title}</span>
            <span className="capabilities__detail">{entry.detail}</span>
          </li>
        ))}
      </ul>
      <p className="capabilities__note">
        扩展指令接入实机前仍需开发调试接口。
      </p>
    </section>
  );
}
