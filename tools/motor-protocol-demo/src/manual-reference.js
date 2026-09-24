// Manual V1.0.5 CAN reply interpreter for raw board traces.
//
// Source of truth: ZDT_X42S 二代闭环步进电机用户手册 V1.0.5 (pages cited per
// decoder). Firmware baseline is X firmware, so every position/speed/error
// scaling below is the X firmware one (0.1° / 0.1 RPM / 0.01°). The manual
// gives different frames for the Emm firmware; this module never detects or
// converts between firmware types, it only says which firmware the numbers
// assume.
//
// Decoding rules (deliberately strict — a wrong guess is worse than nothing):
//   * RX frames only, extended identifiers only, remote frames ignored.
//   * CAN id = (address << 8) | packetIndex: address lives in bits 8..15 and
//     the packet index in bits 0..7 (manual p42). decodeCanReply handles only
//     packet 0; decodeBulkCanReply validates all five packets of 0x42/0x43.
//   * the identifier must be a valid extended id (id <= 0xFFFF) with address
//     1..255.
//   * the data bytes must be bytes, have exactly the documented length, end
//     with the fixed 0x6B checksum, and carry only documented sign/reserved
//     values. Anything else returns null instead of a partial guess.
//   * unknown opcodes return null. A read payload that happens to start with
//     02 is never reported as a command acknowledgement.
//
// Pure module: no React, no timers, no network.

import { PROTOCOL_CHECKSUM, hexByte } from './protocol.js';

// ---------------------------------------------------------------------------
// Frame plumbing
// ---------------------------------------------------------------------------

function toByteArray(value) {
  if (!Array.isArray(value)) return null;
  const bytes = [];
  for (const entry of value) {
    if (!Number.isInteger(entry) || entry < 0 || entry > 0xff) return null;
    bytes.push(entry);
  }
  return bytes;
}

const u16 = (hi, lo) => (hi << 8) | lo;
const u32 = (b0, b1, b2, b3) => b0 * 0x1000000 + (b1 << 16) + (b2 << 8) + b3;

// Sign byte 00/01 = positive/negative for 0x32/0x33/0x34/0x35/0x36/0x37
// (manual p70-p73). 0x39 inverts it: 00/01 = negative/positive (p72).
// Any other value is not a documented sign, so the reply is not decoded.
const signIsValid = (value) => value === 0 || value === 1;

function signedMagnitude(signByte, magnitude, inverted = false) {
  const positive = inverted ? signByte === 1 : signByte === 0;
  return positive ? magnitude : -magnitude;
}

const oneDecimal = (value) => (value / 10).toFixed(1);
const twoDecimals = (value) => (value / 100).toFixed(2);
const binary = (value) => value.toString(2).padStart(8, '0');

// Reply text is line-oriented: one short value line, then short annotations.
// The device page renders it with white-space: pre-line and flattens the same
// lines into single-line trace notes.
const lines = (...parts) => parts.filter(Boolean).join('\n');
const cite = (pages) => `手册 V1.0.5 ${pages}`;

// Repeated on every firmware-scaled field on purpose: the manual scales these
// differently for Emm, and this tool never auto-detects the firmware.
const X_ONLY = '本页按 X 固件解释，不自动识别固件类型';

const bitLine = (value, bit, label, interpretation) =>
  `${label} = ${(value >> bit) & 1}（${interpretation}）`;

// ---------------------------------------------------------------------------
// Bit tables (short labels: one line per bit)
// ---------------------------------------------------------------------------

// Manual p73-p74. The limit bits are input pin levels, not "limit triggered",
// and bit7 is the power-loss flag that reads 0 again after a reboot.
const MOTOR_STATUS_BITS = [
  { bit: 0, label: 'Ens_TF 使能', off: '未使能', on: '已使能' },
  { bit: 1, label: 'Prf_TF 位置到达', off: '未到达', on: '已到达输入目标位置' },
  { bit: 2, label: 'Cgi_TF 堵转', off: '未置位', on: '已置位' },
  { bit: 3, label: 'Cgp_TF 堵转保护', off: '未触发', on: '已触发' },
  { bit: 4, label: 'Esi_LF 左限位输入引脚', off: '低电平', on: '高电平' },
  { bit: 5, label: 'Esi_RF 右限位输入引脚', off: '低电平', on: '高电平' },
  { bit: 7, label: 'Oac_TF 掉电标志', off: '默认值，掉电重启后恢复 0', on: '已置位，可由命令设置' },
];

const MOTOR_STATUS_CAUTION = '限位位只是输入引脚电平，不代表限位已经触发。';

// Manual p62-p63.
const HOMING_STATUS_BITS = [
  { bit: 0, label: 'Enc_Rdy 编码器就绪', off: '编码器异常', on: '编码器正常' },
  { bit: 1, label: 'Cal_Rdy 校准表就绪', off: '未校准', on: '已校准' },
  { bit: 2, label: 'Org_SF 正在回零', off: '否', on: '是' },
  { bit: 3, label: 'Org_CF 回零失败', off: '否', on: '是' },
  { bit: 4, label: 'Otp_TF 过热保护', off: '未触发', on: '已触发' },
  { bit: 5, label: 'Ocp_TF 过流保护', off: '未触发', on: '已触发' },
];

const renderBits = (value, table) =>
  table.map((entry) => bitLine(value, entry.bit, entry.label, (value >> entry.bit) & 1 ? entry.on : entry.off));

// Manual p63: status & 0x0C. 0x00 is the power-on default, so it must never be
// reported as "this homing attempt completed"; 0x0C means both flags are set
// at once and is reported as-is instead of falling through to the 0x00 text.
function homingProgress(flags) {
  const state = flags & 0x0c;
  if (state === 0x04) return '当前正在回零（status & 0x0C = 0x04）';
  if (state === 0x08) return '回零失败（status & 0x0C = 0x08）';
  if (state === 0x0c) return '正在回零与回零失败标志同时为 1（status & 0x0C = 0x0C）：手册未描述该组合，仅原样上报，不能判断回零成功';
  return '未在回零且未报失败（status & 0x0C = 0x00）；这也是上电默认值，不能据此确认本次回零已经完成';
}

// ---------------------------------------------------------------------------
// Read replies ("5.5 读取系统参数命令", manual p67-p75)
//
// Each entry fixes an exact data length, and decode() may return null when a
// documented byte carries an undocumented value (for example sign = 02).
// ---------------------------------------------------------------------------

const READ_REPLIES = [
  {
    opcode: 0x1f,
    length: 6, // [1F][固件版本 u16][硬件版本 u16][6B]
    title: '读取固件版本和硬件版本',
    decode: (d) => lines(
      `固件版本 ${u16(d[1], d[2])}（例 200 = V2.0.0） · 硬件版本原始值 ${u16(d[3], d[4])}`,
      '硬件版本的位表拆分为 HW_Series/HW_Type/HW_Ver，没有独立字节图，这里只报原始 16 位',
      cite('p67'),
    ),
  },
  {
    opcode: 0x20,
    length: 6, // [20][相电阻 u16][相电感 u16][6B]
    title: '读取相电阻和相电感',
    decode: (d) => lines(`相电阻 ${u16(d[1], d[2])} mΩ · 相电感 ${u16(d[3], d[4])} uH`, cite('p67')),
  },
  {
    opcode: 0x24,
    length: 4, // [24][总线电压 u16][6B]
    title: '读取总线电压',
    decode: (d) => lines(`总线电压 ${u16(d[1], d[2])} mV`, 'V+ 经反接二极管后的电压', cite('p68')),
  },
  {
    opcode: 0x26,
    length: 4, // [26][总线电流 u16][6B]
    title: '读取总线电流',
    decode: (d) => lines(
      `总线电流 ${u16(d[1], d[2])} mA`,
      '由相电流和相电压换算，手册注明可能有偏差，仅作趋势参考',
      cite('p68'),
    ),
  },
  {
    opcode: 0x27,
    length: 4, // [27][相电流 u16][6B]
    title: '读取相电流',
    decode: (d) => lines(`相电流 ${u16(d[1], d[2])} mA`, '电机实际工作电流', cite('p69')),
  },
  {
    opcode: 0x31,
    length: 4, // [31][线性化编码器值 u16][6B]
    title: '读取线性化校准后的编码器值',
    decode: (d) => {
      const raw = u16(d[1], d[2]);
      return lines(
        `线性化编码器值 ${raw}（0-65535 = 0-360°）≈ ${((raw / 65536) * 360).toFixed(2)}°`,
        '单圈绝对值：到 65535 后重新从 0 开始',
        cite('p69'),
      );
    },
  },
  {
    opcode: 0x32,
    length: 7, // [32][符号][输入脉冲数 u32][6B]
    title: '读取输入脉冲数',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], u32(d[2], d[3], d[4], d[5]));
      return lines(
        `输入脉冲数 ${value}`,
        '手册注明默认 16 细分下 3200 个脉冲表示一圈 360°，细分改变后需重新换算',
        cite('p70'),
      );
    },
  },
  {
    opcode: 0x33,
    length: 7, // [33][符号][电机目标位置 u32][6B]
    title: '读取电机目标位置',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], u32(d[2], d[3], d[4], d[5]));
      return lines(`目标位置 ${value} → ${oneDecimal(value)}°`, 'X 固件：角度 = 值 / 10', X_ONLY, cite('p70'));
    },
  },
  {
    opcode: 0x34,
    length: 7, // [34][符号][设定目标位置 u32][6B]
    title: '读取电机实时设定的目标位置',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], u32(d[2], d[3], d[4], d[5]));
      return lines(
        `设定目标位置 ${value} → ${oneDecimal(value)}°`,
        'X 固件：角度 = 值 / 10',
        '符号字节按手册解释为开环模式实时位置的正/负',
        X_ONLY,
        cite('p71'),
      );
    },
  },
  {
    opcode: 0x35,
    length: 5, // [35][符号][实时转速 u16][6B]
    title: '读取电机实时转速',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], u16(d[2], d[3]));
      return lines(`实时转速 ${value} → ${oneDecimal(value)} RPM`, 'X 固件：0.1 RPM/计数，原始范围 0000-1388', X_ONLY, cite('p71'));
    },
  },
  {
    opcode: 0x36,
    length: 7, // [36][符号][实时位置 u32][6B]
    title: '读取电机实时位置',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], u32(d[2], d[3], d[4], d[5]));
      return lines(
        `实时位置 ${value} → ${oneDecimal(value)}°`,
        'X 固件：角度 = 值 / 10（0.1°/计数），手册对回包只给出这一种换算',
        X_ONLY,
        cite('p72'),
      );
    },
  },
  {
    opcode: 0x37,
    length: 7, // [37][符号][位置误差 u32][6B]
    title: '读取电机位置误差',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], u32(d[2], d[3], d[4], d[5]));
      return lines(
        `位置误差 ${value} → ${twoDecimals(value)}°`,
        'X 固件：角度 = 值 / 100（0.01°/计数），比位置读取的 0.1° 细一档',
        X_ONLY,
        cite('p73'),
      );
    },
  },
  {
    opcode: 0x39,
    length: 4, // [39][温度符号][温度][6B]
    title: '读取驱动温度',
    decode: (d) => {
      if (!signIsValid(d[1])) return null;
      const value = signedMagnitude(d[1], d[2], true);
      return lines(
        `驱动温度 ${value} ℃`,
        '温度符号：00 = 负数，01 = 正数（与其它带符号读取相反）',
        cite('p72'),
      );
    },
  },
  {
    opcode: 0x3a,
    length: 3, // [3A][电机状态标志][6B]
    title: '读取电机状态标志',
    decode: (d) => lines(
      `电机状态标志 0x${hexByte(d[1])} = 0b${binary(d[1])}`,
      ...renderBits(d[1], MOTOR_STATUS_BITS),
      'bit6 保留',
      MOTOR_STATUS_CAUTION,
      cite('p73-p74'),
    ),
  },
  {
    opcode: 0x3b,
    length: 3, // [3B][回零状态标志][6B]
    title: '读取回零状态标志',
    decode: (d) => lines(
      `回零状态标志 0x${hexByte(d[1])} = 0b${binary(d[1])}`,
      ...renderBits(d[1], HOMING_STATUS_BITS),
      'bit6/bit7 保留',
      homingProgress(d[1]),
      cite('p62-p63'),
    ),
  },
  {
    opcode: 0x3c,
    length: 4, // [3C][回零状态标志][电机状态标志][6B]
    title: '读取回零状态标志 + 电机状态标志',
    decode: (d) => lines(
      `回零状态标志 0x${hexByte(d[1])} = 0b${binary(d[1])}`,
      ...renderBits(d[1], HOMING_STATUS_BITS),
      homingProgress(d[1]),
      `电机状态标志 0x${hexByte(d[2])} = 0b${binary(d[2])}`,
      ...renderBits(d[2], MOTOR_STATUS_BITS),
      MOTOR_STATUS_CAUTION,
      cite('p74'),
    ),
  },
  {
    opcode: 0x3d,
    length: 3, // [3D][引脚 IO 电平状态][6B]
    title: '读取引脚 IO 电平状态',
    decode: (d) => {
      const flags = d[1];
      if ((flags & 0b1010) !== 0) return null; // bit1/bit3 are documented as 0
      return lines(
        `引脚 IO 电平 0x${hexByte(flags)} = 0b${binary(flags)}`,
        bitLine(flags, 0, 'En_Pin 使能引脚', flags & 1 ? '高电平' : '低电平'),
        bitLine(flags, 2, 'Stp_Pin 脉冲引脚', flags & 4 ? '高电平' : '低电平'),
        bitLine(flags, 4, 'Dir_Pin 方向引脚', flags & 16 ? '高电平' : '低电平'),
        bitLine(flags, 5, 'Dir_OM 方向引脚模式', flags & 32 ? '输出模式' : '输入模式'),
        'bit1、bit3 恒为 0；bit6、bit7 保留',
        cite('p75'),
      );
    },
  },
];

const READ_BY_OPCODE = new Map(READ_REPLIES.map((entry) => [entry.opcode, entry]));

// ---------------------------------------------------------------------------
// Control replies ("4.1.2 电机返回命令格式说明", manual p40)
//
// Control commands answer with exactly [opcode][status][6B]. Read commands use
// their own table above, so a read payload that starts with 02 can never be
// mistaken for an acknowledgement. 0x42/0x43 bulk reads and the Y42 battery
// read are intentionally absent: bulk replies need reassembly, while battery
// telemetry applies to Y42 rather than the X42S target of this page.
// ---------------------------------------------------------------------------

const CONTROL_COMMANDS = new Map([
  [0x06, '编码器校准'],
  [0x08, '重启'],
  [0x0a, '当前位置角度清零'],
  [0x0e, '解除堵转/过热/过流保护'],
  [0x0f, '恢复出厂设置'],
  [0x45, '修改闭环模式最大电流'],
  [0x46, '修改开环/闭环控制模式'],
  [0x4c, '修改回零参数'],
  [0x50, '修改掉电标志'],
  [0x84, '修改细分值'],
  [0x93, '设置单圈零点'],
  [0x9a, '触发回零'],
  [0x9c, '中断回零'],
  [0xae, '修改电机 ID/地址'],
  [0xc5, '力矩模式限速控制'],
  [0xc6, '速度模式限电流控制'],
  [0xcb, '直通限速位置模式限电流控制'],
  [0xcd, '梯形曲线加减速位置模式限电流控制'],
  [0xd4, '修改电机运动正方向'],
  [0xd5, '修改固件类型'],
  [0xd7, '修改电机类型'],
  [0xf1, '快速位置模式配置'],
  [0xf3, '使能/关闭使能'],
  [0xf5, '力矩模式控制'],
  [0xf6, '速度模式控制'],
  [0xfb, '直通限速位置模式控制'],
  [0xfc, '快速位置目标'],
  [0xfd, '梯形曲线加减速位置模式控制'],
  [0xfe, '立即停止'],
  [0xff, '广播触发已缓存运动'],
]);

// Manual p40 lists the three completion replies explicitly.
const COMPLETION_TEXT = new Map([
  [0xf5, '力矩模式夹爪夹紧返回'],
  [0xfb, '到位返回'],
  [0xfd, '到位返回'],
  [0x9a, '回零完成返回'],
]);

// 12/22 are only explained by the manual for homing (9A). For any other
// command they stay a raw status byte instead of an invented meaning.
function statusLine(opcode, status) {
  if (status === 0x12 || status === 0x22) {
    if (opcode === 0x9a) {
      return '触发回零时当前已经在零点处或左/右限位已经触发，电机不动（手册把 12 和 22 写在一起，没有分别对应左还是右）';
    }
    return `状态字节 0x${hexByte(status)}：手册只在回零（9A）语境下解释 12/22，本命令的含义未说明，仅原样上报`;
  }
  switch (status) {
    case 0x02: return '接收的命令正确——只代表收到了命令，不代表运动或回零已经完成';
    case 0xe2: return '接收的命令参数错误、数据范围不满足或执行条件不满足（含低压警告、堵转保护、过流/过热保护等情况）';
    case 0xee: return '接收的命令格式错误';
    case 0x9f: return '动作执行完成（电机主动返回）；是否返回取决于 Response / “控制命令应答”配置';
    default: return null;
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Decode one raw board CAN trace entry into a manual-grounded explanation.
 *
 * @param {{dir:'RX'|'TX', id:number, extended:boolean, remote:boolean, data:number[]}} frame
 * @returns {{title:string, text:string, opcode:number, address:number}|null}
 *   null whenever the frame is not a single, complete, documented reply.
 */
export function decodeCanReply(frame) {
  if (!frame || typeof frame !== 'object') return null;
  if (frame.dir !== 'RX') return null;
  if (frame.extended !== true) return null;
  if (frame.remote) return null;

  const { id } = frame;
  if (!Number.isInteger(id) || id < 0 || id > 0xffff) return null;
  if ((id & 0xff) !== 0) return null; // packet index 0 only, no reassembly
  const address = (id >> 8) & 0xff;
  if (address < 1 || address > 255) return null;

  const data = toByteArray(frame.data);
  if (!data || data.length < 3) return null;
  if (data[data.length - 1] !== PROTOCOL_CHECKSUM) return null;

  const opcode = data[0];

  const read = READ_BY_OPCODE.get(opcode);
  if (read) {
    if (data.length !== read.length) return null;
    const text = read.decode(data);
    if (!text) return null; // undocumented sign/reserved byte value
    return { title: read.title, text, opcode, address };
  }

  const commandName = CONTROL_COMMANDS.get(opcode);
  if (commandName) {
    if (data.length !== 3) return null;
    const status = data[1];
    const statusText = statusLine(opcode, status);
    if (!statusText) return null;
    const completion = status === 0x9f ? COMPLETION_TEXT.get(opcode) : null;
    return {
      title: `${commandName} 返回`,
      text: lines(
        `状态字节 0x${hexByte(status)}`,
        completion ? `${statusText}：${completion}` : statusText,
        cite('p40'),
      ),
      opcode,
      address,
    };
  }

  return null;
}

// The 0x42/0x43 X-firmware replies contain 37 logical bytes including the
// address. CAN carries seven payload bytes per frame and repeats the opcode in
// every packet. Decode only a complete consecutive run; a partial trace must
// never be presented as confirmed driver settings.
export function decodeBulkCanReply(packets) {
  if (!Array.isArray(packets) || packets.length !== 5) return null;
  const first = packets[0];
  if (!first || first.dir !== 'RX' || !first.extended || first.remote ||
      !Number.isInteger(first.id) || first.id < 0x100 || first.id > 0xff00 ||
      (first.id & 0xff) !== 0) return null;
  const address = first.id >> 8;
  const opcode = first.data?.[0];
  if (opcode !== 0x42 && opcode !== 0x43) return null;
  const payload = [];
  for (let i = 0; i < packets.length; i++) {
    const packet = packets[i];
    const data = toByteArray(packet?.data);
    if (packet?.dir !== 'RX' || packet.extended !== true || packet.remote ||
        packet.id !== first.id + i || !data || data.length !== 8 || data[0] !== opcode)
      return null;
    payload.push(...data.slice(1));
  }
  if (payload.length !== 35 || payload[0] !== 0x25 || payload[1] !== (opcode === 0x42 ? 0x18 : 0x0c) ||
      payload.at(-1) !== PROTOCOL_CHECKSUM) return null;
  const signed = (sign, start, divisor) => {
    if (!signIsValid(sign)) return null;
    return (signedMagnitude(sign, u32(...payload.slice(start,start+4))) / divisor).toFixed(divisor === 100 ? 2 : 1);
  };
  let title, text;
  if (opcode === 0x42) {
    const response = ['不返回', 'Receive：只返回接收确认', 'Reached：只返回完成', 'Both：接收确认和完成', 'Other：位置完成、其余接收确认'][payload[23]];
    title = '读取全部驱动配置（X 固件）';
    text = lines(
      `按键锁定 ${payload[2]} · 控制模式 ${payload[3] === 1 ? '闭环' : payload[3] === 0 ? '开环' : `原始值 ${payload[3]}`}`,
      `脉冲端口复用代码 ${payload[4]} · 通讯端口复用代码 ${payload[5]} · En 引脚有效电平代码 ${payload[6]} · Dir 引脚有效电平代码 ${payload[7]}`,
      `细分 ${payload[8]} · 细分插补 ${payload[9]} · 自动息屏 ${payload[10]} · 保留字节 0x${hexByte(payload[11])}`,
      `开环模式工作电流 ${u16(payload[12],payload[13])} mA`,
      `闭环模式最大电流 ${u16(payload[14],payload[15])} mA`,
      `闭环模式最大速度 ${u16(payload[16],payload[17])} RPM · 电流环带宽 ${u16(payload[18],payload[19])} Hz`,
      `串口波特率代码 ${payload[20]} · CAN 通讯速率代码 ${payload[21]} · 通讯校验代码 ${payload[22]}`,
      `控制命令应答 ${response ?? `原始值 ${payload[23]}`} · 角度缩小 10 倍输入 ${payload[24]}`,
      `堵转保护代码 ${payload[25]} · 检测转速 ${u16(payload[26],payload[27])} RPM · 检测电流 ${u16(payload[28],payload[29])} mA`,
      `堵转检测时间 ${u16(payload[30],payload[31])} ms · 到位窗口 ${(u16(payload[32],payload[33])/10).toFixed(1)}°`,
      '以上为完整五包读回；代码值未查表转换，可对照底部原始帧',
      cite('p99-p103'),
    );
  } else {
    const target = signed(payload[12],13,10);
    const speed = signIsValid(payload[17]) ? (signedMagnitude(payload[17],u16(payload[18],payload[19]))/10).toFixed(1) : null;
    const position = signed(payload[20],21,10);
    const error = signed(payload[25],26,100);
    // Bulk status sign5 uses 00/01 = positive/negative (manual p96), unlike
    // the standalone 0x39 temperature reply on p72.
    const temperature = signIsValid(payload[30]) ? signedMagnitude(payload[30],payload[31]) : null;
    if ([target,speed,position,error,temperature].some(value => value === null)) return null;
    title = '读取全部系统状态（X 固件）';
    text = lines(
      `总线电压 ${u16(payload[2],payload[3])} mV · 总线电流 ${u16(payload[4],payload[5])} mA · 相电流 ${u16(payload[6],payload[7])} mA`,
      `编码器原始值 ${u16(payload[8],payload[9])} · 线性化编码器值 ${u16(payload[10],payload[11])}`,
      `目标位置 ${target}° · 实时转速 ${speed} RPM · 实时位置 ${position}° · 位置误差 ${error}°`,
      `驱动温度 ${temperature} ℃ · 回零标志 0x${hexByte(payload[32])} · 电机标志 0x${hexByte(payload[33])}`,
      X_ONLY,
      cite('p95-p97'),
    );
  }
  return {title,text,opcode,address};
}
