#include "dependency_network_def.h"

#include <iostream>
#include <stdexcept>
#include <linux/net.h>

bool Dependency_Network_Target::operator==(
		const Dependency_Network_Target &rhs) {
	return (this->ip == rhs.ip) && (this->port == rhs.port);
}

bool Dependency_Network_Target::operator!=(
		const Dependency_Network_Target &rhs) {
	return (this->ip != rhs.ip) || (this->port != rhs.port);
}

template<typename T>
std::vector<T> getMemoryValues(CPUState *cpu, uint32_t addr, uint32_t size) {
	std::vector<T> arguments;
	
	// An array of raw memory bytes. Since we want to fetch T elements from
	// memory and we need enough bytes to store a single instance of T, so the 
	// array's size is equivalent to the size of a single T.
	uint8_t raw[sizeof(T)];
	
	for (auto i = 0; i < size; ++i) {
		// For each argument, read the bytes from memory and store them into
		// the raw memory array. Cast the raw memory to T and store in the 
		// arguments list.
		panda_virtual_memory_rw(cpu, addr + i * sizeof(T), raw, sizeof(T), 0);
		T argument = *(reinterpret_cast<T*>(raw));
		arguments.push_back(argument);
	}
	
	return arguments;
}

void labelBufferContents(CPUState *cpu, target_ulong vAddr, uint32_t length) {
	if (!taint2_enabled()) return;
	if (dependency_network.debug) {
		std::cout << "dependency_network: labeling " << length << " bytes " <<
			"starting from virtual address " << vAddr << "." << std::endl;
	}
	
	int bytesTainted = 0; // Number of bytes that were tainted
	for (auto i = 0; i < length; ++i) {
		// Convert the virtual address to a physical, assert it is valid, if
		// not skip this byte.
		hwaddr pAddr = panda_virt_to_phys(cpu, vAddr + i);
		if (pAddr == (hwaddr)(-1)) {
			std::cerr << "dependency_network: unable to taint at address: " <<
				vAddr << " (virtual), " << pAddr << " (physical)." << 
				std::endl;
			continue;
		}
		// Else, taint at the physical address specified
		taint2_label_ram(pAddr, 1);
		++bytesTainted;
	}
	
	if (dependency_network.debug) {
		std::cout << "dependency_network: labeled " << bytesTainted << 
			" out of " << length << " bytes at virtual address " << vAddr << 
			std::endl;
	}
}

int on_before_block_translate(CPUState *cpu, target_ulong pc) {
	// Enable taint if current instruction is g.t. when we are supposed to
	// enable taint.
	int instr = rr_get_guest_instr_count();
	if (!taint2_enabled() && instr > dependency_network.enableTaintAt) {
		if (dependency_network.debug) {
			std::cout << "dependency_network: enabling taint at instruction " 
				<< instr << "." << std::endl;
				
		}
		
		taint2_enable_taint();
	}
	
	return 0;
}

void on_pread64_return(CPUState *cpu, target_ulong pc, uint32_t fd,
		uint32_t buffer, uint32_t count, uint64_t pos) {
	Dependency_Network_Target target;
	try {
		target = targets.at(std::make_pair(panda_current_asid(cpu), fd));
	} catch (const std::out_of_range &e) {
		std::cerr << "dependency_network: pread64_return called, but file"
			<< "descriptor " << fd << " is unknown." << std::endl;
		return;
	}
	
	if (target == dependency_network.source) {
		std::cout << "dependency_network: ***saw read return of source " << 
			"target***" << std::endl;
		sawReadOfSource = true;
		
		labelBufferContents(cpu, buffer, count);
	} else {
		if (dependency_network.debug) {
			std::cout << "dependency_network: saw read of file/socket with " <<
				"fd: " << fd << std::endl;
		}
	}
}

void on_pwrite64_return(CPUState *cpu, target_ulong pc, uint32_t fd,
		uint32_t buffer, uint32_t count, uint64_t pos) {
	Dependency_Network_Target target;
	try {
		target = targets.at(std::make_pair(panda_current_asid(cpu), fd));
	} catch (const std::out_of_range &e) {
		std::cerr << "dependency_network: pwrite64_return called, but file" <<
			" descriptor " << fd << " is unknown." << std::endl;
		return;
	}
	
	if (target == dependency_network.sink) {
		std::cout << "dependency_network: ***saw write return of sink " << 
			"target***" << std::endl; 
		sawWriteOfSink = true;
		
		int numTainted = queryBufferContents(cpu, buffer, count);
		std::cout << "dependency_file: " << numTainted << " tainted bytes " <<
			"written to " << target.ip << "." << std::endl;

		if (numTainted > 0) dependency = true;
	} else {
		if (dependency_network.debug) {
			std::cout << "dependency_network: saw write of file/socket with " 
				<< "fd: " << fd << std::endl;
		}
	}
}

void on_read_return(CPUState *cpu, target_ulong pc, uint32_t fd, 
		uint32_t buffer, uint32_t count) {
	on_pread64_return(cpu, pc, fd, buffer, count, 0);
}

void on_socketcall_return(CPUState *cpu, target_ulong pc, int32_t call,
		uint32_t args) {
	if (dependency_network.debug) {
		std::cout << "dependency_network: socket_call triggered at " << 
			"instruction " << rr_get_guest_instr_count() << ", call type: " <<
			call << std::endl;
	}
			
	switch (call) {
	case SYS_CONNECT:
		on_socketcall_connect_return(cpu, args);
		return;
	case SYS_SEND:
	case SYS_SENDTO:
		on_socketcall_send_return(cpu, args);
		return;
	case SYS_RECV:
	case SYS_RECVFROM:
		on_socketcall_recv_return(cpu, args);
		return;
	}
}

void on_socketcall_connect_return(CPUState *cpu, uint32_t args) {
	std::cout << "dependency_network: socket_connect called at " <<
		rr_get_guest_instr_count() << "." << std::endl;
	
	// Get the arguments from the args virtual memory
	auto arguments = getMemoryValues<uint32_t>(cpu, args, 3);

	// Get the sockaddr structure from the arguments. The virtual memory 
	// address to the sockaddr structure is stored in the second argument of
	// the args passed to connect(). Use that address to get the pointer to the
	// sockaddr structure itself.
	auto sockaddrAddress = arguments[1];
	sockaddr addr = getMemoryValues<sockaddr>(cpu, sockaddrAddress, 1)[0];
	
	// Stores the IP address found and the port number
	char ipAddress[INET6_ADDRSTRLEN] = {0};
	unsigned short port = 0;
	
	if (addr.sa_family == AF_INET) {
		sockaddr_in *sin4 = reinterpret_cast<sockaddr_in*>(&addr);
		
		inet_ntop(AF_INET, &sin4->sin_addr, ipAddress, INET6_ADDRSTRLEN);
		port = sin4->sin_port;
		
	} else if (addr.sa_family == AF_INET6) {
		sockaddr_in6 *sin6 = reinterpret_cast<sockaddr_in6*>(&addr);
		
		inet_ntop(AF_INET6, &sin6->sin6_addr, ipAddress, INET6_ADDRSTRLEN);
		port = sin6->sin6_port;
	} else {
		std::cerr << "dependency_network: sockaddr fetched but is of an " <<
			"unknown family." << std::endl;
		return;
	}
	
	// Convert IP address & port to Dependency_Network_Target. Add the Target
	// to the targets map.
	int sockfd = arguments[0];
	Dependency_Network_Target target = { std::string(ipAddress), port };
	targets[std::make_pair(panda_current_asid(cpu), sockfd)] = target;
	
	// Print IP address and port
	if (dependency_network.debug) {
		std::cout << "dependency_network: connect called for target IP: " << 
			target.ip << ", and target port: " << target.port << std::endl;
	}

	// If a connection to a source or sink is detected, turn on tainting
	// to be ready to intercept read/write and send/recv.
	if (target == dependency_network.source) {
		std::cout << "***saw connect to source target***" << std::endl;
		dependency_network.enableTaintAt = rr_get_guest_instr_count();
	} else if (target == dependency_network.sink) {
		std::cout << "***saw connect to sink target***" << std::endl;
		dependency_network.enableTaintAt = rr_get_guest_instr_count();
	}
}

void on_socketcall_recv_return(CPUState *cpu, uint32_t args) {
	std::cout << "dependency_network: socket_recv called at " <<
		rr_get_guest_instr_count() << "." << std::endl;
	
	// Get the arguments from the args virtual memory
	auto arguments = getMemoryValues<uint32_t>(cpu, args, 4);
	
	// Retrieve the socket file descriptor, buffer address and buffer length
	// from the arguments.
	uint32_t sockfd = arguments[0];
	uint32_t buffer = arguments[1];
	uint32_t length = arguments[2];
	
	// Try to get the network target using the socket file descriptor
	Dependency_Network_Target target;
	try {
		target = targets.at(std::make_pair(panda_current_asid(cpu), sockfd));
	} catch (const std::out_of_range &e) {
		std::cerr << "dependency_network: socket_recv called, but file" <<
			" descriptor " << sockfd << " is unknown." << std::endl;
		return;
	}
	
	// If we are receiving information from the source target, taint it
	if (target == dependency_network.source) {
		std::cout << "dependency_network: ***saw recv from source target***" 
			<< std::endl;
			
		sawReadOfSource = true;
		labelBufferContents(cpu, buffer, length);
	}
}

void on_socketcall_send_return(CPUState *cpu, uint32_t args) {
	std::cout << "dependency_network: socket_recv called at " <<
		rr_get_guest_instr_count() << "." << std::endl;
	
	// Get the arguments from the args virtual memory
	auto arguments = getMemoryValues<uint32_t>(cpu, args, 4);
	
	// Retrieve the socket file descriptor, buffer address and buffer length
	// from the arguments.
	uint32_t sockfd = arguments[0];
	uint32_t buffer = arguments[1];
	uint32_t length = arguments[2];
	
	// Try to get the network target using the socket file descriptor
	Dependency_Network_Target target;
	try {
		target = targets.at(std::make_pair(panda_current_asid(cpu), sockfd));
	} catch (const std::out_of_range &e) {
		std::cerr << "dependency_network: socket_send called, but file" <<
			" descriptor " << sockfd << " is unknown." << std::endl;
		return;
	}
	
	// If we are receiving information from the source target, taint it
	if (target == dependency_network.sink) {
		std::cout << "dependency_network: ***saw send to sink target***" << 
			std::endl;
			
		int numTainted = queryBufferContents(cpu, buffer, length);
		std::cout << "dependency_network: " << numTainted << " tainted bytes " 
			<< "written to " << target.ip << "." << std::endl;

		sawWriteOfSink = true;
		if (numTainted > 0) dependency = true;
	}
}

void on_write_return(CPUState *cpu, target_ulong pc, uint32_t fd, 
		uint32_t buffer, uint32_t count) {
	on_pwrite64_return(cpu, pc, fd, buffer, count, 0);
}

int queryBufferContents(CPUState *cpu, target_ulong vAddr, uint32_t length) {
	if (!taint2_enabled()) return -1;
	if (dependency_network.debug) {
		std::cout << "dependency_network: querying " << length << " bytes " <<
			"starting from virtual address " << vAddr << "." << std::endl;
	}
	
	int bytesWithTaint = 0; // Number of bytes which were tainted
	for (auto i = 0; i < length; ++i) {
		// Convert the virtual address to a physical, assert it is valid, if
		// not skip this byte.
		hwaddr pAddr = panda_virt_to_phys(cpu, vAddr + i);
		if (pAddr == (hwaddr)(-1)) {
			std::cerr << "dependency_network: unable to query at address: " <<
				vAddr << " (virtual), " << pAddr << " (physical)." << 
				std::endl;
			continue;
		}
		// Else, query the taint, increment counter if tainted
		uint32_t cardinality = taint2_query_ram(pAddr);
		if (cardinality > 0) ++bytesWithTaint;
	}
	
	if (dependency_network.debug) {
		std::cout << "dependency_network: found " << bytesWithTaint << 
			" tainted bytes out of " << length << " at virtual address " <<
			vAddr << std::endl;
	}
	return bytesWithTaint;
}

bool init_plugin(void *self) {
#ifdef TARGET_I386
	dependency_network.plugin_ptr = self;
	
	/// Load dependent plugins
	panda_require("osi");
	assert(init_osi_api());
	
	panda_require("osi_linux");
	assert(init_osi_linux_api());
	
	panda_require("syscalls2");
	
	panda_require("taint2");
	assert(init_taint2_api());
	
	/// Parse Arguments:
	/// "source_ip"   : The source IP address, defaults to "0.0.0.0"
	/// "source_port" : The source port, defaults to "9999"
	/// "sink_ip"     : The sink IP address, defaults to "0.0.0.0"
	/// "sink_port"   : The sink port, defaults to "9999"
	/// "debug"       : Should debug mode be used? Defaults to false
	auto args = panda_get_args("dependency_network");
	dependency_network.source.ip = panda_parse_string_opt(args, 
		"source_ip", "0.0.0.0", "source ip address");
	dependency_network.source.port = (unsigned short)panda_parse_uint32_opt(
		args, "source_port", 9999, "source port number");
	dependency_network.sink.ip = panda_parse_string_opt(args, 
		"sink_ip", "0.0.0.0", "sink ip address");
	dependency_network.sink.port = (unsigned short)panda_parse_uint32_opt(
		args, "sink_port", 9999, "sink port number");
	dependency_network.debug = panda_parse_bool_opt(args, 
		"debug", "debug mode");
	std::cout << "dependency_network: source IP: " << 
		dependency_network.source.ip << std::endl;
	std::cout << "dependency_network: source port: " << 
		dependency_network.source.port << std::endl;
	std::cout << "dependency_network: sink IP: " << 
		dependency_network.sink.ip << std::endl;
	std::cout << "dependency_network: sink port: " << 
		dependency_network.sink.port << std::endl;
	std::cout << "dependency_network: debug: " << 
		dependency_network.debug << std::endl;
	
	// Register SysCalls2 Callback Functions
	PPP_REG_CB("syscalls2", on_sys_socketcall_return, on_socketcall_return);
	PPP_REG_CB("syscalls2", on_sys_pread64_return, on_pread64_return);
	PPP_REG_CB("syscalls2", on_sys_pwrite64_return, on_pwrite64_return);
	PPP_REG_CB("syscalls2", on_sys_read_return, on_read_return);
	PPP_REG_CB("syscalls2", on_sys_write_return, on_write_return);
	
	// Register the Before Block Execution Functions
	panda_cb pcb;
	pcb.before_block_translate = on_before_block_translate;
	panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_TRANSLATE, pcb);
	
	return true;
#else
	std::cout << "dependency_network is only supported for i386 targets." << 
		std::endl;
	return false;
#endif
}

void uninit_plugin(void *self) {
	std::cout << "dependency_network: saw read of source? " << 
		sawReadOfSource << std::endl;
	std::cout << "dependency_network: saw write of sink? " << 
		sawWriteOfSink << std::endl;
	std::cout << "dependency_network: saw dependency? " << 
		dependency << std::endl;
}
