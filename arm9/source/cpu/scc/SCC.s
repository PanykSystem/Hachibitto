;@
;@
;@  SCC.s
;@  Konami SCC+/K052539 sound chip emulator for arm32.
;@
;@  Created by Fredrik Ahlström on 2006-04-01.
;@  Copyright © 2006-2024 Fredrik Ahlström. All rights reserved.
;@
;@  What's new (SCC+ support and fast mixer):
;@    - SCC+: Ch4 has its own waveform (wave RAM spans 0x00-0x9F), and the
;@      register block lives at 0xA0-0xAF, mirrored at 0xB0-0xBF.
;@      SCCRead returns 0xFF for 0xA0 and up (write-only registers).
;@    - sccChControl (0xAF, channel enable/"key on") is enforced.
;@    - Mixer rewritten for speed and for high notes: the old counter
;@      could only advance one wave step per sample, which broke any
;@      note above roughly SCC clock / (32 * SCCMULT) once SCCMULT was
;@      raised to 128. The new design has no such limit.
;@    - Frequency period is now F+1 ticks, as on the real chip.
;@    - Save-state load now replays raw register values. The old code
;@      replayed translated volumes, which came back wrong.
;@
;@  Mixer design:
;@    - Each channel has a 32-bit phase accumulator (top 5 bits = wave
;@      index, low 27 bits = fraction) and a 32-bit per-sample increment.
;@    - The increment is computed when the frequency register is written:
;@          inc = ((SCCMULT<<22)/(F+1))<<5     (0 if F<16, channel frozen)
;@      so the mixer does just "phase += inc; idx = phase>>27" per channel
;@      and handles any number of wave steps per sample at constant cost.
;@      Periods below 16 ticks (above ~7 kHz) are frozen instead of
;@      mixed, since they would only alias at this sample rate.
;@    - Per-channel "effective volume" (table volume, or 0 if the channel
;@      is disabled in sccChControl) is recomputed when a volume register
;@      or the key-on register (0xAF) is written, so the mixer does no
;@      per-sample enable/volume gating.
;@    - The raw register bytes stay in the struct at 0xA0-0xAF (stored by
;@      SCCWrite itself) and are the source of truth for save/load state.
;@    - Mixer loop scheduling: Ch0-2 increments are held in registers for
;@      the whole call, wave bytes are addressed as [r2 + (phase>>27) +
;@      channel offset] so no per-channel wave base registers are needed,
;@      and loads are interleaved with unrelated work to avoid ARM9
;@      load-use stalls. Output is bit-identical to the simple version.
;@
;@  SCCMULT = chip ticks per output sample = 3579545 / output sample rate.
;@
#ifdef __arm__

#include "SCC.i"

#if !defined(SCCMULT)
	#define SCCMULT 16
#endif

	.global SCCReset
	.global SCCSaveState
	.global SCCLoadState
	.global SCCGetStateSize
	.global SCCMixer
	.global SCCWrite
	.global SCCRead


	.syntax unified
	.arm

;@----------------------------------------------------------------------------
;@ Computes one channel's effective volume.
;@ In: r0=chControl, r2=SCCptr, r3=&SCCVolume. Clobbers r1.
;@----------------------------------------------------------------------------
	.macro sccEffCh n
	ldrb r1,[r2,#sccCh\n\()Volume]
	and r1,r1,#0x0F
	ldrb r1,[r3,r1]
	tst r0,#(1<<\n)
	moveq r1,#0
	strb r1,[r2,#sccCh\n\()Eff]
	.endm

	.section .itcm
	.align 2

;@----------------------------------------------------------------------------
;@ r0  = Mix length.
;@ r1  = Mixerbuffer.
;@ r2  = sccptr.
;@ r3 -> r7 = Ch0-Ch4 phase accumulators.
;@ r8  = Increment / volume.
;@ r9  = Mixer sum.
;@ r10 = Ch1 wave base, r11 = Ch2 wave base, r12 = Ch3 wave base.
;@ lr  = Scrap (wave index / sample).
;@----------------------------------------------------------------------------

SCCMixer:					;@ r0=len, r1=dest, r2=SCCptr
	.type   SCCMixer STT_FUNC
;@----------------------------------------------------------------------------
	stmfd sp!,{r4-r11,lr}

	add r8,r2,#sccCh0Phase
	ldmia r8,{r3-r7}
	ldr r10,[r2,#sccCh0Inc]		;@ Ch0-2 increments live in registers
	ldr r11,[r2,#sccCh1Inc]
	ldr r12,[r2,#sccCh2Inc]

;@----------------------------------------------------------------------------
sccMixLoop:
	ldrb r9,[r2,#sccCh0Eff]
	add r3,r3,r10
	ldrb r8,[r2,#sccCh1Eff]
	add lr,r2,r3,lsr#27
	ldrsb lr,[lr]				;@ Channel 0 (wave at +0x00)
	add r4,r4,r11				;@ Ch1 phase (fills load delay)
	mul r9,lr,r9

	add lr,r2,r4,lsr#27
	ldrsb lr,[lr,#0x20]			;@ Channel 1
	add r5,r5,r12				;@ Ch2 phase (fills load delay)
	mla r9,r8,lr,r9

	ldrb r8,[r2,#sccCh2Eff]
	add lr,r2,r5,lsr#27
	ldrsb lr,[lr,#0x40]			;@ Channel 2
	subs r0,r0,#1				;@ fills load delay; flags survive to the end
	mla r9,r8,lr,r9

	ldr lr,[r2,#sccCh3Inc]
	ldrb r8,[r2,#sccCh3Eff]
	add r6,r6,lr
	add lr,r2,r6,lsr#27
	ldrsb lr,[lr,#0x60]			;@ Channel 3
	mla r9,r8,lr,r9

	ldr lr,[r2,#sccCh4Inc]
	ldrb r8,[r2,#sccCh4Eff]
	add r7,r7,lr
	add lr,r2,r7,lsr#27
	ldrsb lr,[lr,#0x80]			;@ Channel 4
	mla r9,r8,lr,r9

	strhpl r9,[r1],#2
	bhi sccMixLoop

	add r8,r2,#sccCh0Phase
	stmia r8,{r3-r7}

	ldmfd sp!,{r4-r11,lr}
	bx lr

;@----------------------------------------------------------------------------

	.section .text
	.align 2

;@----------------------------------------------------------------------------
SCCReset:					;@ r0=SCCptr
	.type   SCCReset STT_FUNC
;@----------------------------------------------------------------------------
	stmfd sp!,{r0,lr}
	mov r1,#0
	mov r2,#sccSize
	bl memset					;@ clear variables
	ldmfd sp!,{r0,lr}

	bx lr

;@----------------------------------------------------------------------------
SCCSaveState:					;@ In r0=destination, r1=SCCptr. Out r0=state size.
	.type   SCCSaveState STT_FUNC
;@----------------------------------------------------------------------------
	add r1,r1,#sccStateStart
	mov r2,#sccStateEnd-sccStateStart
	stmfd sp!,{r2,lr}
	bl memcpy
	ldmfd sp!,{r0,lr}
	bx lr

;@----------------------------------------------------------------------------
SCCLoadState:					;@ In r0=SCCptr, r1=source. Out r0=state size.
	.type   SCCLoadState STT_FUNC
;@----------------------------------------------------------------------------
	stmfd sp!,{r4,r5,lr}

	mov r4,r0
	add r0,r0,#sccStateStart
	mov r2,#sccStateEnd-sccStateStart
	bl memcpy

	mov r5,#0xF

stateLoop:
	add r2,r4,#sccStateStart
	add r1,r5,#0xA0
	ldrb r0,[r2,r1]				;@ raw register byte

	mov r2,r4
	bl SCCWrite					;@ replays: rebuilds Inc and Eff fields

	subs r5,r5,#1
	bpl stateLoop

	ldmfd sp!,{r4,r5,lr}

;@----------------------------------------------------------------------------
SCCGetStateSize:				;@ Out r0=state size.
	.type   SCCGetStateSize STT_FUNC
;@----------------------------------------------------------------------------
	mov r0,#sccStateEnd-sccStateStart
	bx lr

;@----------------------------------------------------------------------------
SCCVolume:
	.byte 0,3,7,10,14,17,20,24,27,31,34,37,41,44,48,51

;@----------------------------------------------------------------------------
sccUpdateEff:					;@ r2=SCCptr. Recompute all 5 effective volumes.
;@ Used for volume writes (0xAA-0xAE) and key-on writes (0xAF).
;@----------------------------------------------------------------------------
	ldrb r0,[r2,#sccChControl]
	adr r3,SCCVolume
	sccEffCh 0
	sccEffCh 1
	sccEffCh 2
	sccEffCh 3
	sccEffCh 4
	bx lr

;@----------------------------------------------------------------------------
SCCRead:					;@ 0x9800-0x9FFF, r0=adr, r1=SCCptr
	.type   SCCRead STT_FUNC
;@----------------------------------------------------------------------------
	and r0,r0,#0xFF
	cmp r0,#0xA0
	ldrblo r0,[r1,r0]
	movhs r0,#0xFF
	bx lr

;@----------------------------------------------------------------------------
SCCWrite:					;@ 0x9800-0x9FFF, r0=val, r1=adr, r2=SCCptr
	.type   SCCWrite STT_FUNC
;@----------------------------------------------------------------------------
	and r1,r1,#0xFF

	cmp r1,#0xB0
	subpl r1,r1,#0x10

	cmp r1,#0xB0

	add r3,r2,#sccCh0Wave

	strbmi r0,[r3,r1]			;@ raw byte into struct (wave RAM + regs 0xA0-0xAF)
	strbpl r0,[r2,#sccTestReg]

	bxpl lr

	subs r1,r1,#0xA0
	ldrpl pc,[pc,r1,lsl#2]
	bx lr

	.long sccFreq0				;@ 0xA0 Ch0 freq low
	.long sccFreq0				;@ 0xA1 Ch0 freq high
	.long sccFreq1
	.long sccFreq1
	.long sccFreq2
	.long sccFreq2
	.long sccFreq3
	.long sccFreq3
	.long sccFreq4
	.long sccFreq4
	.long sccUpdateEff			;@ 0xAA Ch0 volume
	.long sccUpdateEff			;@ 0xAB Ch1 volume
	.long sccUpdateEff			;@ 0xAC Ch2 volume
	.long sccUpdateEff			;@ 0xAD Ch3 volume
	.long sccUpdateEff			;@ 0xAE Ch4 volume
	.long sccUpdateEff			;@ 0xAF channel enable / key-on

;@----------------------------------------------------------------------------
sccFreq0:
	mov r1,#0
	b sccUpdateInc
sccFreq1:
	mov r1,#1
	b sccUpdateInc
sccFreq2:
	mov r1,#2
	b sccUpdateInc
sccFreq3:
	mov r1,#3
	b sccUpdateInc
sccFreq4:
	mov r1,#4
;@ fall through

;@----------------------------------------------------------------------------
sccUpdateInc:					;@ r1=channel (0-4), r2=SCCptr
;@ inc = ((SCCMULT<<22)/(F+1))<<5, or 0 if F<16 (too fast, channel frozen).
;@ F is the 12-bit frequency from the raw register bytes at 0xA0+2*ch.
;@----------------------------------------------------------------------------
	stmfd sp!,{r2,r4,r5,lr}		;@ 16 bytes, keeps stack 8-byte aligned
	add r4,r2,r1,lsl#2
	add r4,r4,#sccCh0Inc			;@ r4 = &inc[ch]
	add r3,r2,r1,lsl#1
	ldrh r1,[r3,#sccCh0Frq]
	bic r1,r1,#0xF000				;@ F = low 12 bits
	mov r0,#0
	cmp r1,#16
	blo 1f
	add r1,r1,#1					;@ divisor = F+1
	ldr r0,=(SCCMULT<<22)
	bl __aeabi_uidiv				;@ or your hardware divider wrapper
	mov r0,r0,lsl#5
1:
	str r0,[r4]
	ldmfd sp!,{r2,r4,r5,lr}
	bx lr
	.ltorg

;@----------------------------------------------------------------------------
	.end
#endif // #ifdef __arm__
