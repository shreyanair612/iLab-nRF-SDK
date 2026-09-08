import Foundation
import CoreBluetooth

let nusServiceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let nusTXUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

private let sampleRate: UInt32 = 16_000
private let channelCount: UInt16 = 1
private let bitsPerSample: UInt16 = 16
private let transferIdleTimeout: TimeInterval = 1.0

final class BLEReceiver: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let outputDirectory = URL(fileURLWithPath: "/Users/shreybae/Documents/iLAB/nRF_SDK/12_bt-bf-vad/bt_processing")
    private let logURL: URL
    private let packetLogURL: URL
    private let pcmURL: URL
    private let wavURL: URL

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var logHandle: FileHandle?
    private var packetLogHandle: FileHandle?
    private var pcmHandle: FileHandle?
    private var wavHandle: FileHandle?
    private var completionTimer: Timer?

    private var packetNumber = 0
    private var totalPCMBytes: UInt32 = 0
    private var transferActive = false

    override init() {
        logURL = outputDirectory.appendingPathComponent("ble_receiver.log")
        packetLogURL = outputDirectory.appendingPathComponent("ble_receiver.txt")
        pcmURL = outputDirectory.appendingPathComponent("audio.pcm")
        wavURL = outputDirectory.appendingPathComponent("audio.wav")

        super.init()

        prepareTextFile(at: logURL, handle: &logHandle)
        prepareTextFile(at: packetLogURL, handle: &packetLogHandle)

        log("=== BLE NUS receiver started ===")
        log("Packet log: \(packetLogURL.path)")
        log("Raw PCM: \(pcmURL.path)")
        log("WAV audio: \(wavURL.path)")

        central = CBCentralManager(delegate:self, queue:nil)
    }

    deinit {
        completionTimer?.invalidate()
        try? logHandle?.close()
        try? packetLogHandle?.close()
        try? pcmHandle?.close()
        try? wavHandle?.close()
    }

    private func prepareTextFile(at url: URL, handle: inout FileHandle?) {
        FileManager.default.createFile(atPath: url.path, contents:nil)
        handle = try? FileHandle(forWritingTo: url)
        try? handle?.seekToEnd()
    }

    private func log(_ message: String) {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"

        let line = "\(formatter.string(from: Date())) \(message)\n"

        print(line, terminator: "")

        if let data = line.data(using: .utf8) {
            try? logHandle?.write(contentsOf:data)
        }
    }

    private func beginTransferIfNeeded() {
        guard !transferActive else { return }

        transferActive = true
        packetNumber = 0
        totalPCMBytes = 0

        try? pcmHandle?.close()
        try? wavHandle?.close()

        FileManager.default.createFile(atPath: pcmURL.path, contents:nil)
        FileManager.default.createFile(atPath: wavURL.path, contents:nil)

        pcmHandle = try? FileHandle(forWritingTo: pcmURL)
        wavHandle = try? FileHandle(forWritingTo: wavURL)

        let placeholderHeader = wavHeader(dataByteCount: 0)
        try? wavHandle?.write(contentsOf: placeholderHeader)

        log("Receiving audio packet stream...")
    }

    private func appendAudioPacket(_ data: Data) {
        packetNumber += 1;
        totalPCMBytes += UInt32(data.count)

        try? pcmHandle?.write(contentsOf: data)
        try? wavHandle?.write(contentsOf: data)

        let hex = data.map { String(format: "%02X", $0) }.joined(separator: " ")
        let line = String(
            format: "%.3f packet=%d bytes=%d total_pcm_bytes=%u %s\n",
            Date().timeIntervalSince1970, packetNumber, data.count,
            totalPCMBytes, hex
        )


        if let text = line.data(using: .utf8) {
            try? packetLogHandle?.write(contentsOf: text)
        }
    }

    private func resetCompletionTimer() {
        completionTimer?.invalidate();
        completionTimer = Timer.scheduledTimer(withTimeInterval: transferIdleTimeout,
        repeats: false) { [weak self] _ in 
            self?.finishTransfer() 
        }
    }

    private func finishTransfer() {
        guard transferActive else { return }

        transferActive = false
        completionTimer?.invalidate()
        completionTimer = nil

        guard totalPCMBytes <= UInt32.max - 36 else {
            log("ERROR: WAV data exceeds RIFF/WAV 4 GB size limit")
            return
        }

        do {
            try wavHandle?.seek(toOffset:0)
            try wavHandle?.write(contentsOf: wavHeader(dataByteCount: totalPCMBytes))
            try wavHandle?.synchronize()
            try pcmHandle?.synchronize()
            try wavHandle?.close()
            try pcmHandle?.close()
            wavHandle = nil
            pcmHandle = nil

            let duration = Double(totalPCMBytes) / Double(sampleRate * UInt32(channelCount) * UInt32(bitsPerSample / 8))

            log(String(format: 
                "Transfer complete. packets=%d pcm_bytes=%u duration=%.3f s",
                packetNumber, totalPCMBytes, duration
            ))
            log("Packet data saved to \(packetLogURL.lastPathComponent)")
            log("Raw PCM saved to \(pcmURL.lastPathComponent)")
            log("WAV audio saved to \(wavURL.lastPathComponent)")
        } catch {
            log("ERROR: Could not finalize WAV file: \(error.localizedDescription)")
        }
    }

    private func wavHeader(dataByteCount: UInt32) -> Data {
        let byteRate = sampleRate * UInt32(channelCount) * UInt32(bitsPerSample/8)
        let blockAlign = channelCount * (bitsPerSample/8)
        let riffSize = 36+dataByteCount

        var data = Data()
        data.append("RIFF".data(using: .ascii)!)
        data.appendLE(riffSize)
        data.append("WAVE".data(using: .ascii)!)
        data.append("fmt ".data(using: .ascii)!)
        data.appendLE(UInt32(16))
        data.appendLE(UInt16(1))
        data.appendLE(channelCount)
        data.appendLE(sampleRate)
        data.appendLE(byteRate)
        data.appendLE(blockAlign)
        data.appendLE(bitsPerSample)
        data.append("data".data(using: .ascii)!)
        data.appendLE(dataByteCount)
        return data
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            log("Bluetooth is not ready: \(central.state.rawValue)")
            return
        }
        log("Bluetooth ready. Scanning for Nordic UART Service (NUS)...")
        central.scanForPeripherals(withServices: [nusServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard self.peripheral == nil else {return}

        self.peripheral = peripheral
        peripheral.delegate = self

        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? 
            String ?? peripheral.name ?? "unknown"

        log("Found device: \(name), RSSI: \(RSSI), Connecting...")
        central.stopScan()
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("Connected. Looking for NUS service...")
        peripheral.discoverServices([nusServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        log("ERROR: Connection failed: " + (error?.localizedDescription ?? "unknown error"))
        self.peripheral = nil
        central.scanForPeripherals(withServices: [nusServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("Disconnected: \(error?.localizedDescription ?? "no error")")
        self.peripheral = nil
        central.scanForPeripherals(withServices: [nusServiceUUID])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            log("ERROR: Service discovery failed: " + error.localizedDescription)
            return
        }

        guard let service = peripheral.services?.first(where: { $0.uuid == nusServiceUUID }) else {
            log("ERROR: Nordic UART Service was not found.")
            return
        }

        log("NUS service found. Discovering TX characteristic...")
        peripheral.discoverCharacteristics([nusTXUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error = error {
            log("ERROR: Characteristic discovery failed: \(error.localizedDescription)")
            return
        }

        guard let characteristic = service.characteristics?.first(where: {$0.uuid == nusTXUUID }) else {
            log("ERROR: NUS TX characteristic not found")
            return
        }

        log("NUS TX properties raw value: " + String(characteristic.properties.rawValue))

        if !characteristic.properties.contains(.notify) {
            log("ERROR: NUS TX characteristic does not suppoer notifications")
            return
        }

        peripheral.setNotifyValue(true, for: characteristic)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            log("ERROR: Could not enable notifications: \(error.localizedDescription)")
        } else if characteristic.isNotifying {
            log("Notifications enabled. Waiting for packets...")
        } else {
            log("Notifications disabled.")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            log("ERROR: Packet receive failed: \(error.localizedDescription)")
            return
        }

        guard characteristic.uuid == nusTXUUID, let data = characteristic.value, !data.isEmpty else { return }
        
        beginTransferIfNeeded()
        appendAudioPacket(data)
        resetCompletionTimer()
    }
}

private extension Data {
    mutating func appendLE(_ value: UInt16) {
        var littleEndianValue = value.littleEndian
        append(Data(bytes: &littleEndianValue, count: MemoryLayout<UInt16>.size))
    }

    mutating func appendLE(_ value: UInt32) {
        var littleEndianValue = value.littleEndian
        append(Data(bytes: &littleEndianValue, count: MemoryLayout<UInt32>.size))
    }
}

let receiver = BLEReceiver()
RunLoop.main.run()