import { formatBytes, formatCanId } from '../protocol.js';

export function FrameList({ frames, annotations, onCopy, showSplitNote = true }) {
  if (frames.length === 0) {
    return <p className="preview__empty">逻辑指令无效或长度不足，暂不生成 CAN 帧。</p>;
  }

  return (
    <>
      {frames.map((frame) => (
        <div className="frame" key={frame.packetIndex}>
          <p className="frame__meta">
            扩展帧 · Packet {frame.packetIndex} · DLC {frame.dlc}
          </p>
          <div className="frame__box">
            <span className="frame__id">ID {formatCanId(frame.canId)}</span>
            <span className="frame__data">DATA {formatBytes(frame.data)}</span>
            <button
              type="button"
              className="link-button frame__copy"
              onClick={() => onCopy(
                `ID ${formatCanId(frame.canId)} DLC ${frame.dlc} DATA ${formatBytes(frame.data)}`,
                `Packet ${frame.packetIndex}`,
              )}
            >
              复制
            </button>
          </div>
          <p className="frame__note">{annotations[frame.packetIndex]}</p>
        </div>
      ))}

      {showSplitNote && frames.length > 1 ? (
        <p className="frame__split">
          共 {frames.length} 帧：按 sendCommand 规则去掉地址与功能码后，每 7 字节一组，每组前面补功能码，
          ID = (地址 &lt;&lt; 8) | 包序号。
        </p>
      ) : null}
    </>
  );
}
