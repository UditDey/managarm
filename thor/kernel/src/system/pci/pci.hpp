#ifndef THOR_SYSTEM_PCI_PCI_HPP
#define THOR_SYSTEM_PCI_PCI_HPP

#include <stddef.h>
#include <stdint.h>
#include <frigg/smart_ptr.hpp>
#include <frigg/vector.hpp>
#include "../../generic/irq.hpp"

namespace thor {

struct Memory;
struct IoSpace;

namespace pci {

enum class IrqIndex {
	null, inta, intb, intc, intd
};

inline const char *nameOf(IrqIndex index) {
	switch(index) {
	case IrqIndex::inta: return "INTA";
	case IrqIndex::intb: return "INTB";
	case IrqIndex::intc: return "INTC";
	case IrqIndex::intd: return "INTD";
	default:
		assert(!"Illegal PCI interrupt pin for nameOf(IrqIndex)");
	}
}

inline const char *nameOfCapability(unsigned int type) {
	switch(type) {
	case 0x04: return "Slot-identification";
	case 0x05: return "MSI";
	case 0x09: return "Vendor-specific";
	case 0x0A: return "Debug-port";
	case 0x10: return "PCIe";
	case 0x11: return "MSI-X";
	default:
		return nullptr;
	}
}

struct RoutingEntry {
	unsigned int slot;
	IrqIndex index;
	IrqPin *pin;
};

using RoutingInfo = frigg::Vector<RoutingEntry, KernelAlloc>;

struct PciDevice {
	enum BarType {
		kBarNone = 0,
		kBarIo = 1,
		kBarMemory = 2
	};

	struct Bar {
		Bar()
		: type(kBarNone), address(0), length(0), offset(0) { }

		BarType type;
		uintptr_t address;
		size_t length;
		
		frigg::SharedPtr<Memory> memory;
		frigg::SharedPtr<IoSpace> io;
		ptrdiff_t offset;
	};

	struct Capability {
		unsigned int type;
		ptrdiff_t offset;
		size_t length;
	};

	PciDevice(uint32_t bus, uint32_t slot, uint32_t function,
			uint32_t vendor, uint32_t device_id, uint8_t revision,
			uint8_t class_code, uint8_t sub_class, uint8_t interface)
	: mbusId(0), bus(bus), slot(slot), function(function),
			vendor(vendor), deviceId(device_id), revision(revision),
			classCode(class_code), subClass(sub_class), interface(interface),
			interrupt(nullptr), caps(*kernelAlloc) { }
	
	// mbus object ID of the device
	int64_t mbusId;

	// location of the device on the PCI bus
	uint32_t bus;
	uint32_t slot;
	uint32_t function;
	
	// vendor-specific device information
	uint16_t vendor;
	uint16_t deviceId;
	uint8_t revision;

	// generic device information
	uint8_t classCode;
	uint8_t subClass;
	uint8_t interface;

	IrqPin *interrupt;
	
	// device configuration
	Bar bars[6];

	frigg::Vector<Capability, KernelAlloc> caps;
};

enum {
	// general PCI header fields
	kPciVendor = 0,
	kPciDevice = 2,
	kPciCommand = 4,
	kPciStatus = 6,
	kPciRevision = 0x08,
	kPciInterface = 0x09,
	kPciSubClass = 0x0A,
	kPciClassCode = 0x0B,
	kPciHeaderType = 0x0E,

	// usual device header fields
	kPciRegularBar0 = 0x10,
	kPciRegularSubsystemVendor = 0x2C,
	kPciRegularSubsystemDevice = 0x2E,
	kPciRegularCapabilities = 0x34,
	kPciRegularInterruptLine = 0x3C,
	kPciRegularInterruptPin = 0x3D,

	// PCI-to-PCI bridge header fields
	kPciBridgeSecondary = 0x19
};

void pciDiscover(const RoutingInfo &routing);

} } // namespace thor::pci

// read from pci configuration space
uint32_t readPciWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset);
uint16_t readPciHalf(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset);
uint8_t readPciByte(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset);

// write to pci configuration space
void writePciWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint32_t value);
void writePciHalf(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint16_t value);
void writePciByte(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint8_t value);

#endif // THOR_SYSTEM_PCI_PCI_HPP
