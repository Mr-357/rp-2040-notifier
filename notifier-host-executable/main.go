package main

import (
	"fmt"
	"os"
	"strings"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// -----------------------------------------------------------------------------
// USB identity
// -----------------------------------------------------------------------------

const (
	notifierVID = "CAFE"
	notifierPID = "1337"
)

// -----------------------------------------------------------------------------
// Protocol
// -----------------------------------------------------------------------------

const (
	cmdReady     byte = 0x01
	cmdDone      byte = 0x02
	cmdAttention byte = 0x03
	cmdError     byte = 0x04

	cmdIdentify byte = 0x7F
)

const (
	deviceSignature = "LLM-NOTIFIER/1"
	baudRate        = 115200
)

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

func main() {
	// Only argv[1] belongs to us. Anything after it is deliberately ignored.
	// This is useful for integrations such as Codex that may append an event
	// payload to the configured notification command.
	if len(os.Args) < 2 {
		failUsage()
	}

	command, err := parseCommand(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	debugf("requested action %q -> command 0x%02X\n", os.Args[1], command)

	portName, err := findNotifier()
	if err != nil {
		debugf("notifier discovery failed: %v\n", err)

		// The physical notifier is optional. Do not make Claude/Codex/OpenCode
		// fail just because the device is unplugged or unavailable.
		return
	}

	debugf("using notifier on %s\n", portName)

	port, err := openPort(portName)
	if err != nil {
		debugf("failed opening notifier on %s: %v\n", portName, err)
		return
	}
	defer port.Close()

	debugf("sending command 0x%02X to %s\n", command, portName)

	if _, err := port.Write([]byte{command}); err != nil {
		debugf("failed sending command to %s: %v\n", portName, err)
		return
	}

	if err := port.Drain(); err != nil {
		debugf("failed draining %s: %v\n", portName, err)
		return
	}

	debugf("command sent successfully\n")
}

// -----------------------------------------------------------------------------
// Command parsing
// -----------------------------------------------------------------------------

func parseCommand(value string) (byte, error) {
	switch strings.ToLower(value) {
	case "ready", "1":
		return cmdReady, nil
	case "done", "2":
		return cmdDone, nil
	case "attention", "3":
		return cmdAttention, nil
	case "error", "4":
		return cmdError, nil
	default:
		return 0, fmt.Errorf(
			"unknown action %q; expected ready, done, attention, error, 1, 2, 3, or 4",
			value,
		)
	}
}

func failUsage() {
	fmt.Fprintln(
		os.Stderr,
		"usage: agent-notify <ready|done|attention|error>",
	)
	os.Exit(2)
}

// -----------------------------------------------------------------------------
// Serial
// -----------------------------------------------------------------------------

func openPort(name string) (serial.Port, error) {
	mode := &serial.Mode{
		BaudRate: baudRate,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
		InitialStatusBits: &serial.ModemOutputBits{
			DTR: true,
			RTS: false,
		},
	}

	return serial.Open(name, mode)
}

// -----------------------------------------------------------------------------
// Device discovery
// -----------------------------------------------------------------------------

func findNotifier() (string, error) {
	// Optional manual override.
	//
	// PowerShell:
	//   $env:LLM_NOTIFIER_PORT = "COM5"
	//
	// Linux:
	//   export LLM_NOTIFIER_PORT=/dev/ttyACM0
	if override := os.Getenv("LLM_NOTIFIER_PORT"); override != "" {
		debugf("LLM_NOTIFIER_PORT override is set to %s\n", override)
		debugf("probing configured port %s...\n", override)

		ok, err := probeNotifier(override)
		if err != nil {
			return "", fmt.Errorf(
				"could not probe configured port %s: %w",
				override,
				err,
			)
		}

		if !ok {
			return "", fmt.Errorf(
				"configured port %s is not an LLM notifier",
				override,
			)
		}

		debugf("configured port %s identified successfully\n", override)
		return override, nil
	}

	// Manufacturer/Product/Configuration require active USB probing in the
	// serial enumerator. Enable it only in debug mode so normal invocations keep
	// discovery as lightweight as possible.
	var (
		ports []*enumerator.PortDetails
		err   error
	)

	if debugEnabled() {
		ports, err = enumerator.GetDetailedPortsList(enumerator.All)
	} else {
		ports, err = enumerator.GetDetailedPortsList()
	}

	if err != nil {
		return "", fmt.Errorf("could not enumerate serial ports: %w", err)
	}

	debugf("serial devices found: %d\n", len(ports))

	candidates := make([]string, 0)

	for _, port := range ports {
		if port.IsUSB {
			debugf(
				"  %s: USB VID=%s PID=%s manufacturer=%q product=%q serial=%q configuration=%q\n",
				port.Name,
				port.VID,
				port.PID,
				port.Manufacturer,
				port.Product,
				port.SerialNumber,
				port.Configuration,
			)
		} else {
			debugf("  %s: non-USB serial device\n", port.Name)
		}

		if !port.IsUSB {
			continue
		}

		if !strings.EqualFold(port.VID, notifierVID) ||
			!strings.EqualFold(port.PID, notifierPID) {
			continue
		}

		debugf(
			"    -> VID/PID match (%s:%s), adding %s as candidate\n",
			notifierVID,
			notifierPID,
			port.Name,
		)

		candidates = append(candidates, port.Name)
	}

	if len(candidates) == 0 {
		return "", fmt.Errorf(
			"no LLM notifier USB devices found (%s:%s)",
			notifierVID,
			notifierPID,
		)
	}

	debugf("VID/PID candidates: %d\n", len(candidates))

	matches := make([]string, 0)

	for _, candidate := range candidates {
		debugf("probing candidate %s...\n", candidate)

		ok, err := probeNotifier(candidate)
		if err != nil {
			// A candidate may be busy or otherwise inaccessible. Skip it and
			// continue checking any other matching device.
			debugf("  %s: probe failed: %v\n", candidate, err)
			continue
		}

		if ok {
			debugf("  %s: IDENTIFY succeeded\n", candidate)
			matches = append(matches, candidate)
		} else {
			debugf("  %s: IDENTIFY response did not match\n", candidate)
		}
	}

	switch len(matches) {
	case 0:
		return "", fmt.Errorf(
			"USB device(s) with VID:PID %s:%s found, but none identified as an LLM notifier",
			notifierVID,
			notifierPID,
		)
	case 1:
		return matches[0], nil
	default:
		return "", fmt.Errorf(
			"multiple LLM notifiers found: %s",
			strings.Join(matches, ", "),
		)
	}
}

// -----------------------------------------------------------------------------
// Identification
// -----------------------------------------------------------------------------

func probeNotifier(name string) (bool, error) {
	port, err := openPort(name)
	if err != nil {
		return false, err
	}
	defer port.Close()

	// Give the CDC connection a moment to settle after opening.
	time.Sleep(50 * time.Millisecond)

	if err := port.ResetInputBuffer(); err != nil {
		return false, err
	}

	if err := port.SetReadTimeout(100 * time.Millisecond); err != nil {
		return false, err
	}

	// Try IDENTIFY more than once. This makes discovery tolerant of a
	// just-enumerated device where the first byte arrives slightly too early.
	for attempt := 1; attempt <= 3; attempt++ {
		debugf("    IDENTIFY attempt %d/3 on %s\n", attempt, name)

		if _, err := port.Write([]byte{cmdIdentify}); err != nil {
			return false, err
		}

		if err := port.Drain(); err != nil {
			return false, err
		}

		response, err := readResponse(port, 350*time.Millisecond)
		if err != nil {
			return false, err
		}

		if response != "" {
			debugf("    response from %s: %q\n", name, response)
		} else {
			debugf("    no response from %s\n", name)
		}

		if strings.Contains(response, deviceSignature) {
			return true, nil
		}

		time.Sleep(50 * time.Millisecond)
	}

	return false, nil
}

func readResponse(port serial.Port, timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)
	var response strings.Builder
	buffer := make([]byte, 64)

	for time.Now().Before(deadline) {
		n, err := port.Read(buffer)
		if err != nil {
			return "", err
		}

		if n == 0 {
			continue
		}

		response.Write(buffer[:n])

		if strings.Contains(response.String(), deviceSignature) {
			break
		}
	}

	return response.String(), nil
}

// -----------------------------------------------------------------------------
// Debug output
// -----------------------------------------------------------------------------

func debugEnabled() bool {
	return os.Getenv("LLM_NOTIFIER_DEBUG") == "1"
}

func debugf(format string, args ...any) {
	if !debugEnabled() {
		return
	}

	fmt.Fprintf(os.Stderr, format, args...)
}
