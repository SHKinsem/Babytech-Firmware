# Independent review checkpoints

- rotationDistance[1]=8 mm/rev, move1 +10mm => +450 degrees => CD distance 00001194 (4500 tenths). rotationDistance[2]=40 => same10mm =90deg (900 tenths). Negative input changes direction, not unsigned field.
- velocity3 -30 RPM,1000ms,accel60,current800 => logical `03 C6 01 00 3C 01 2C 00 03 20 6B`. CAN first packet ID300 DATA `C6 01 00 3C 01 2C 00 03`, secondID301 DATA `C6 20 6B`.
- torque3 -800mA,1000ms,max30RPM,ramp1000 => logical `03 C5 01 03 E8 03 20 00 01 2C 6B`. FirstID300 DATA `C5 01 03 E8 03 20 00 01`, secondID301 DATA `C5 2C 6B`.
- Raw `hex 00 FF 66 6B` broadcasts synchronous trigger even though protected lab whitelist rejects it; no fake ACK/completion. Raw `can std 123 DE AD` is actualID123 DLC2, not translated motorID123.
- Stage1 initial compile found tests reference private cpp kFrameHome; use independent literal0x9A in tests.
- Stage1 review concern: latch of clearedSeen persists if newer3B reports running again; inferred completion must require current fresh3B notrunning and samples after completion proof. Missing3B must fail homing after freshness window (exceptexplicitcompletion path with freshstationary). Do not allow pre-completion stationary samples to prove completion.
- Stop must set enableDesired=enableConfirmed after cancelling pending enable; otherwise phantom pending desire blocks configuration forever. Preserve confirmed enable.
- Controllerstatus home result has homeId; frontend must not display another target's last home outcome as this motor's.
- Atomic validation means invalid line2 cannot send validline1. Raw step accepted/sent never equivalent to actual bus ACK or movement done.
