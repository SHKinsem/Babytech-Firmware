export async function request(path, data, signal) {
  const controller = new AbortController();
  const abort = () => controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  const timer = setTimeout(abort, 1800);
  try {
    const response = await fetch(path, {
      method: data === undefined ? 'GET' : 'POST', cache: 'no-store',
      body: data === undefined ? undefined : new URLSearchParams(data),
      signal: controller.signal,
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || result.message || `HTTP ${response.status}`);
    return result;
  } finally { clearTimeout(timer); signal?.removeEventListener('abort', abort); }
}

export function supportReason(bytes) {
  if (!bytes?.length) return '请先修正参数';
  const op = bytes[1], word = i => bytes[i]*256 + bytes[i+1];
  if ([0xff, 0xfb, 0xcb, 0xfd, 0x9a].includes(op))
    return '仅预览：此模式尚未接入板端运动监督；相对运动请使用 CD，速度／力矩可作 5 秒试验。';
  if (op === 0xf3 && bytes[4] || op === 0xfe && bytes[3] || [0xf5,0xc5,0xf6,0xc6].includes(op) && bytes[7] || op === 0xcd && bytes[14])
    return '同步队列尚未接入板端监督，请选择立即执行。';
  if (op === 0xcd && bytes[13] !== 2) return '实机相对运动要求 motionMode = 2。';
  if (op === 0x4c && bytes[18]) return '实机不允许配置上电自动回零。';
  if (op === 0x11 && word(4) > 0 && word(4) < 30) return '实机反馈间隔为 0（关闭）或至少 30 ms。';
  if (![0x1f,0x20,0x21,0x24,0x27,0x31,0x33,0x35,0x36,0x37,0x3a,0x3b,0x42,0x43,0xf3,0xfe,0xcd,0xf5,0xc5,0xf6,0xc6,0x9c,0x0a,0x0e,0x46,0x93,0x11,0x4c].includes(op))
    return '未知指令只能预览，板端拒绝发送。';
  return null;
}

export const stateLabels = { idle:'已使能 · 静止', disabled:'未使能', enabled:'已使能', moving:'运动中', stop_requested:'等待停止反馈', enable_pending:'等待使能应答', fault:'故障', experiment_running:'5 秒试验中' };

export const errorLabels = {
  wifi_busy:'Wi-Fi 正忙，请稍后再试', busy:'设备忙，请先停止并关闭使能',
  not_enabled:'尚未收到使能确认', can_unavailable:'CAN 控制器不可用', can_tx_failed:'CAN 发送失败，检查接线与终端电阻',
  feedback_unavailable:'缺少新鲜位置／速度反馈', feedback_stale:'电机反馈中断', stop_pending:'等待真实静止反馈',
  enable_and_wait_for_stationary_feedback:'请先使能，并等待静止反馈',
  disable_and_wait_for_stationary_feedback:'配置前请关闭使能，并等待真实静止反馈',
  unsupported_or_invalid_command:'指令不受支持或超出板端参数范围',
};
