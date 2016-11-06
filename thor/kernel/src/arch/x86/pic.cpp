
#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/mem_space.hpp>
#include <arch/io_space.hpp>

#include "generic/kernel.hpp"

namespace thor {

arch::bit_register<uint32_t> lApicId(0x0020);
arch::scalar_register<uint32_t> lApicEoi(0x00B0);
arch::bit_register<uint32_t> lApicSpurious(0x00F0);
arch::bit_register<uint32_t> lApicIcrLow(0x0300);
arch::bit_register<uint32_t> lApicIcrHigh(0x0310);
arch::bit_register<uint32_t> lApicLvtTimer(0x0320);
arch::scalar_register<uint32_t> lApicInitCount(0x0380);
arch::scalar_register<uint32_t> lApicCurCount(0x0390);

// lApicId registers
arch::field<uint32_t, uint8_t> apicId(24, 8);

// lApicSpurious registers
arch::field<uint32_t, uint8_t> apicSpuriousVector(0, 8);
arch::field<uint32_t, bool> apicSpuriousSwEnable(8, 1);
arch::field<uint32_t, bool> apicSpuriousFocusProcessor(9, 1);
arch::field<uint32_t, bool> apicSpuriousEoiBroadcastSuppression(12, 1);

// lApicIcrLow registers
arch::field<uint32_t, uint8_t> apicIcrLowVector(0, 8);
arch::field<uint32_t, uint8_t> apicIcrLowDelivMode(8, 3);
arch::field<uint32_t, bool> apicIcrLowDestMode(11, 1);
arch::field<uint32_t, bool> apicIcrLowDelivStatus(12, 1);
arch::field<uint32_t, bool> apicIcrLowLevel(14, 1);
arch::field<uint32_t, bool> apicIcrLowTriggerMode(15, 1);
arch::field<uint32_t, uint8_t> apicIcrLowDestShortHand(18, 2);

// lApicIcrHigh registers
arch::field<uint32_t, uint8_t> apicIcrHighDestField(23, 8);

// lApicLvtTimer registers
arch::field<uint32_t, uint8_t> apicLvtVector(0, 8);

arch::mem_space picBase;

enum {
	kModelLegacy = 1,
	kModelApic = 2
};

static int picModel = kModelLegacy;

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

uint32_t *localApicRegs;
uint32_t apicTicksPerMilli;

void initLocalApicOnTheSystem() {
	uint64_t apic_info = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrLocalApicBase);
	assert((apic_info & (1 << 11)) != 0); // local APIC is enabled
	localApicRegs = accessPhysical<uint32_t>(apic_info & 0xFFFFF000);
	picBase = arch::mem_space(localApicRegs);

	frigg::infoLogger() << "Booting on CPU #" << getLocalApicId() << frigg::endLog;
}

void initLocalApicPerCpu() {
	// enable the local apic
	uint32_t spurious_vector = 0x81;
	picBase.store(lApicSpurious, apicSpuriousVector(spurious_vector)
			| apicSpuriousSwEnable(true));
	
	// setup a timer interrupt for scheduling
	uint32_t schedule_vector = 0x82;
	picBase.store(lApicLvtTimer, apicLvtVector(schedule_vector));
}

uint32_t getLocalApicId() {
	return picBase.load(lApicId) & apicId;
}

uint64_t localTicks() {
	return picBase.load(lApicCurCount);
}

void calibrateApicTimer() {
	const uint64_t millis = 100;
	picBase.store(lApicInitCount, 0xFFFFFFFF);
	pollSleepNano(millis * 1000000);
	uint32_t elapsed = 0xFFFFFFFF
			- picBase.load(lApicCurCount);
	picBase.store(lApicInitCount, 0);
	apicTicksPerMilli = elapsed / millis;
	
	frigg::infoLogger() << "Local elapsed ticks: " << elapsed << frigg::endLog;
}

void preemptThisCpu(uint64_t slice_nano) {
	assert(apicTicksPerMilli > 0);
	
	uint64_t ticks = (slice_nano / 1000000) * apicTicksPerMilli;
	if(ticks == 0)
		ticks = 1;
	picBase.store(lApicInitCount, ticks);
}

void acknowledgePreemption() {
	picBase.store(lApicEoi, 0);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:init = 5, Level:assert = 1, TM:Level = 1
	picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
			| apicIcrLowLevel(true) | apicIcrLowTriggerMode(true));
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:init = 5, TM:Level = 1
	picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
			| apicIcrLowTriggerMode(true));
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	assert((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:startup = 6
	picBase.store(lApicIcrLow, apicIcrLowVector(vector)
			| apicIcrLowDelivMode(6));
}

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

uint32_t *ioApicRegs;

arch::scalar_register<uint32_t> apicIndex(0x00);
arch::scalar_register<uint32_t> apicData(0x10);

enum {
	kIoApicId = 0,
	kIoApicVersion = 1,
	kIoApicInts = 16,
};

uint32_t readIoApic(uint32_t index) {
	auto picIoBase = arch::mem_space(ioApicRegs);	
	picIoBase.store(apicIndex, index);
	return picIoBase.load(apicData);
}
void writeIoApic(uint32_t index, uint32_t value) {
	auto picIoBase = arch::mem_space(ioApicRegs);	
	picIoBase.store(apicIndex, index);
	picIoBase.store(apicData, value);
}

void setupIoApic(PhysicalAddr address) {
	ioApicRegs = accessPhysical<uint32_t>(address);
	
	picModel = kModelApic;
	maskLegacyPic();

	int num_ints = ((readIoApic(kIoApicVersion) >> 16) & 0xFF) + 1;
	frigg::infoLogger() << "I/O APIC supports " << num_ints << " interrupts" << frigg::endLog;

	for(int i = 0; i < num_ints; i++) {
		uint32_t vector = 64 + i;
		writeIoApic(kIoApicInts + i * 2, vector);
		writeIoApic(kIoApicInts + i * 2 + 1, 0);
	}
}

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void ioWait() { }

enum LegacyPicRegisters {
	kPic1Command = 0x20,
	kPic1Data = 0x21,
	kPic2Command = 0xA0,
	kPic2Data = 0xA1
};

enum LegacyPicFlags {
	kIcw1Icw4 = 0x01,
	kIcw1Single = 0x02,
	kIcw1Interval4 = 0x04,
	kIcw1Level = 0x08,
	kIcw1Init = 0x10,

	kIcw4Mode8086 = 0x01,
	kIcw4Auto = 0x02,
	kIcw4BufSlave = 0x08,
	kIcw4BufMaster = 0x0C,
	kIcw4Sfnm = 0x10,

	kPicEoi = 0x20
};

void remapLegacyPic(int offset) {
	// save masks
	uint8_t a1 = frigg::arch_x86::ioInByte(kPic1Data);
	uint8_t a2 = frigg::arch_x86::ioInByte(kPic2Data);

	// start initilization
	frigg::arch_x86::ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic1Data, offset);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	frigg::arch_x86::ioOutByte(kPic1Data, 4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, 2);
	ioWait();

	frigg::arch_x86::ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	frigg::arch_x86::ioOutByte(kPic1Data, a1);
	frigg::arch_x86::ioOutByte(kPic2Data, a2);
}

void setupLegacyPic() {
	remapLegacyPic(64);
}

void maskLegacyPic() {
	frigg::arch_x86::ioOutByte(kPic1Data, 0xFF);
	frigg::arch_x86::ioOutByte(kPic2Data, 0xFF);
}

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

void acknowledgeIrq(int irq) {
	if(picModel == kModelApic) {
		picBase.store(lApicEoi, 0);
	}else if(picModel == kModelLegacy) {
		if(irq >= 8)
			frigg::arch_x86::ioOutByte(kPic2Command, kPicEoi);
		frigg::arch_x86::ioOutByte(kPic1Command, kPicEoi);
	}else{
		assert(!"Illegal PIC model");
	}
}

} // namespace thor
