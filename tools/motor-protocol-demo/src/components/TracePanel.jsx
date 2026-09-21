import { useEffect, useRef } from 'react';

import { formatBytes, formatCanId } from '../protocol.js';
import { GlyphSearch } from './glyphs.jsx';

const FILTERS = [
  { value: 'all', label: '全部' },
  { value: 'TX', label: 'TX' },
  { value: 'RX', label: 'RX' },
];

function formatTime(timestamp) {
  const date = new Date(timestamp);
  const pad = (value, width = 2) => String(value).padStart(width, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`;
}

export function TracePanel({
  device = false,
  records,
  totalCount,
  hiddenCount,
  filter,
  onFilterChange,
  query,
  onQueryChange,
  paused,
  onTogglePause,
  onClear,
  onCopy,
}) {
  const bodyRef = useRef(null);

  useEffect(() => {
    if (paused) return;
    const body = bodyRef.current;
    if (body) body.scrollTop = body.scrollHeight;
  }, [records.length, paused]);

  return (
    <section className="trace" aria-label="收发记录">
      <div className="trace__head">
        <h2 className="trace__title">收发记录</h2>
        <span className="chip chip--soft" title={device ? 'TX 为 TWAI 入队，RX 为总线接收；不代表运动完成' : '本地模拟'}>
          {device ? '真实总线 · TX 入队 / RX 接收' : '模拟数据 · 无真实硬件'}
        </span>

        <div className="trace__filters" role="group" aria-label="按方向筛选">
          {FILTERS.map((entry) => (
            <button
              type="button"
              key={entry.value}
              className={`pill${filter === entry.value ? ' is-active' : ''}`}
              aria-pressed={filter === entry.value}
              onClick={() => onFilterChange(entry.value)}
            >
              {entry.label}
            </button>
          ))}
        </div>

        <label className="checkbox">
          <input type="checkbox" checked={paused} onChange={onTogglePause} />
          <span>暂停显示</span>
        </label>

        <div className="search search--compact">
          <GlyphSearch />
          <input
            className="search__input"
            type="search"
            value={query}
            placeholder="搜索帧内容 / 解析"
            aria-label="搜索收发记录"
            onChange={(event) => onQueryChange(event.target.value)}
          />
        </div>

        <span className="trace__count">
          显示 {records.length} / 共 {totalCount} 条
          {paused && hiddenCount > 0 ? ` · 暂停期间新增 ${hiddenCount} 条` : ''}
        </span>

        <button type="button" className="link-button" onClick={onClear}>
          清空记录
        </button>
      </div>

      <div className="trace__table" role="table" aria-label="CAN 收发记录">
        <div className="trace__row trace__row--head" role="row">
          <span role="columnheader">时间</span>
          <span role="columnheader">方向</span>
          <span role="columnheader">CAN ID</span>
          <span role="columnheader">DLC</span>
          <span role="columnheader">数据</span>
          <span role="columnheader">解析</span>
        </div>

        <div className="trace__body" ref={bodyRef}>
          {records.length === 0 ? (
            <p className="trace__empty">{device ? '等待板端收发记录。' : '暂无模拟记录。'}</p>
          ) : (
            records.map((record) => (
              <div className="trace__row" key={record.id} role="row">
                <span className="trace__cell trace__time" role="cell">{formatTime(record.at)}</span>
                <span
                  className={`trace__cell trace__dir trace__dir--${record.dir.toLowerCase()}`}
                  role="cell"
                >
                  {record.dir}
                </span>
                <span className="trace__cell trace__id" role="cell">{formatCanId(record.canId)}</span>
                <span className="trace__cell trace__dlc" role="cell">{record.dlc}</span>
                <span className="trace__cell trace__data" role="cell">{formatBytes(record.data)}</span>
                <span className="trace__cell trace__note" role="cell">
                  <span className="trace__note-text">{record.note}</span>
                  <button
                    type="button"
                    className="link-button trace__copy"
                    onClick={() => onCopy(
                      `${formatTime(record.at)} ${record.dir} ${formatCanId(record.canId)} DLC ${record.dlc} ${formatBytes(record.data)}`,
                      '记录',
                    )}
                  >
                    复制
                  </button>
                </span>
              </div>
            ))
          )}
        </div>
      </div>

      <p className="trace__foot">
        {device ? '真实总线记录：TX 表示 TWAI 入队，RX 表示总线接收。板端保留最近 48 帧，网页最多保留 400 条；ACK 不代表机械动作完成。' : 'RX 全部由本地仿真生成，ACK 只代表固件收到指令，不代表机械动作完成。'}
      </p>
    </section>
  );
}
