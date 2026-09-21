import { GlyphDot } from './glyphs.jsx';

export const BITRATES = [
  { value: '500', label: 'CAN 500 kbit/s' },
  { value: '1000', label: 'CAN 1 Mbit/s' },
];

export function Toolbar({
  addressDraft,
  addressError,
  addressValue,
  onAddressChange,
  bitrate,
  onBitrateChange,
  connected,
  onToggleConnection,
  onStopAll,
}) {
  return (
    <header className="toolbar">
      <div className="toolbar__brand">
        <span className="toolbar__logo">Babytech</span>
        <span className="toolbar__divider" aria-hidden="true">
          /
        </span>
        <h1 className="toolbar__title">电机协议工作台</h1>
      </div>

      <div className="toolbar__right">
        <span className="toolbar__meta">设计概念 · 示例数据</span>

        <div className={`toolbar__field${addressError ? ' has-error' : ''}`}>
          <label className="toolbar__label" htmlFor="can-address">
            CAN ID
          </label>
          <input
            id="can-address"
            className="input input--mono toolbar__address"
            value={addressDraft}
            onChange={(event) => onAddressChange(event.target.value)}
            inputMode="numeric"
            autoComplete="off"
            aria-invalid={addressError ? 'true' : undefined}
            aria-describedby={addressError ? 'can-address-error' : 'can-address-hint'}
            title="电机地址（十进制 1..255），写入扩展帧 ID 的高字节"
          />
          <span className="toolbar__hex" aria-hidden="true">
            = 0x{addressValue.toString(16).toUpperCase().padStart(2, '0')}
          </span>
          {addressError ? (
            <p className="toolbar__error" id="can-address-error" role="alert">
              {addressError}
            </p>
          ) : null}
        </div>

        <div className="toolbar__field">
          <label className="toolbar__label toolbar__label--hidden" htmlFor="can-bitrate">
            位速率
          </label>
          <select
            id="can-bitrate"
            className="select"
            value={bitrate}
            onChange={(event) => onBitrateChange(event.target.value)}
            title="固件 begin() 仅支持 500 kbit/s 与 1 Mbit/s；演示中该设置只影响显示"
          >
            {BITRATES.map((entry) => (
              <option key={entry.value} value={entry.value}>
                {entry.label}
              </option>
            ))}
          </select>
        </div>

        <div className="toolbar__connection">
          <span className="chip chip--soft" title="本地模拟，不会连接真实硬件">
            模拟模式
          </span>
          <button
            type="button"
            className={`connection${connected ? ' is-on' : ''}`}
            aria-pressed={connected}
            onClick={onToggleConnection}
            title="模拟连接开关：仅切换本地模拟状态，不访问任何硬件或网络"
          >
            <GlyphDot tone={connected ? 'ok' : 'idle'} />
            {connected ? '已连接' : '未连接'}
          </button>
        </div>

        <button type="button" className="button button--danger" onClick={onStopAll}>
          全部停止
        </button>
      </div>

      <span className="sr-only" id="can-address-hint">
        电机地址使用十进制整数 1 到 255，写入扩展帧 ID 的高字节
      </span>
    </header>
  );
}
