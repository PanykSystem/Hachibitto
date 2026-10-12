//
//  SCC.h
//  Konami SCC/K051649 sound chip emulator for arm32.
//
//  Created by Fredrik Ahlström on 2006-04-01.
//  Copyright © 2006-2024 Fredrik Ahlström. All rights reserved.
//

#ifndef SCC_HEADER
#define SCC_HEADER

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
	s8 ch0Wave[32];
	s8 ch1Wave[32];
	s8 ch2Wave[32];
	s8 ch3Wave[32];
	s8 ch4Wave[32];
	u16 ch0Frq;
	u16 ch1Frq;
	u16 ch2Frq;
	u16 ch3Frq;
	u16 ch4Frq;
	u8 ch0Volume;
	u8 ch1Volume;
	u8 ch2Volume;
	u8 ch3Volume;
	u8 ch4Volume;
	u8 chControl;
	u8 testReg;
	u8 padding[3];

	// Runtime mixer state (not saved)
	u32 ch0Phase;		// 0xB4
	u32 ch1Phase;
	u32 ch2Phase;
	u32 ch3Phase;
	u32 ch4Phase;		// 0xC4
	u32 ch0Inc;		// 0xC8
	u32 ch1Inc;
	u32 ch2Inc;
	u32 ch3Inc;
	u32 ch4Inc;		// 0xD8
	u8 ch0Eff;		// 0xDC
	u8 ch1Eff;
	u8 ch2Eff;
	u8 ch3Eff;
	u8 ch4Eff;
	u8 effPadding[3];	// pad to 0xE4
} SCC;			// sizeof == 0xE4

/**
 * Reset/initialize SCC chip.
 * @param  *chip: The SCC chip.
 */
void SCCReset(SCC *chip);

/**
 * Saves the state of the SN76496 chip to the destination.
 * @param  *destination: Where to save the state.
 * @param  *chip: The SN76496 chip to save.
 * @return The size of the state.
 */
int SCCSaveState(void *destination, const SCC *chip);

/**
 * Loads the state of the SN76496 chip from the source.
 * @param  *chip: The SN76496 chip to load a state into.
 * @param  *source: Where to load the state from.
 * @return The size of the state.
 */
int SCCLoadState(SCC *chip, const void *source);

/**
 * Gets the state size of a SN76496.
 * @return The size of the state.
 */
int SCCGetStateSize(void);

/**
 * Runs the sound chip for count number of cycles shifted by "SCC_UPSHIFT",
 * @param  count: Number of samples to generate.
 * @param  *dest: Pointer to buffer where sound is rendered.
 * @param  *chip: The SCC chip.
 */
void SCCMixer(int count, s16 *dest, SCC *chip);

/**
 * Write value to SCC chip.
 * @param  value: value to write.
 * @param  address: The address to write to.
 * @param  *chip: The SCC chip.
 */
void SCCWrite(u8 value, u16 adress, SCC *chip);

/**
 * Write value to SCC chip.
 * @param  address: The address to read from.
 * @param  *chip: The SCC chip.
 * @return The value of the address.
 */
u8 SCCRead(u16 address, SCC *chip);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SCC_HEADER
