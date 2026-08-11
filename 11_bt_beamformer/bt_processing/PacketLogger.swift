import Foundation
import CoreBluetooth

let nusServiceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let nusTXUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
let logURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    .appendingPathComponent("nrf_ble_packet_log.txt")

final class PacketLogger: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var packetNumber = 0
    private var totalBytes = 0
    private var logHandle: FileHandle?
    private var packetLogHandle: FileHandle?
    private var completionTimer: Timer?
    private var transferActive = false

    override init() {
        FileManager.default.createFile(atPath: logURL.path, contents:nil)
        logHandle = try? FileHandle(forWritingTo: logURL)
        logHandle?.seekToEndOfFile()

        FileManager.default.createFile(
            atPath:URL(fileURLWithPath:FileManager.default.currentDirectoryPath)
                .appendingPathComponent("nrf_ble_packet_log.txt").path,
            contents: nil
        )

        let packetLogURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            .appendingPathComponent("nrf_ble_packet_log.txt")

        packetLogHandle = try? FileHandle(forWritingTo: packetLogURL)
        packetLogHandle?.seekToEndOfFile()

        super.init()
        log("=== BLE NUS Packet Logger started ===")
        log("Log file: \(logURL.path)")
        central = CBCentralManager(delegate: self, queue: nil)
    }

    private func log(_ message: String) {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        let line = "\(formatter.string(from: Date())) \(message)"
        print(line)
        if let data = (line + "\n").data(using: .utf8) {
            logHandle?.write(data)
        }
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
            String ?? peripheral.name ?? "<unnamed>"
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
            log("ERROR: The discovered 0003 characteristic does not advertise Notify.")
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
        guard let data = characteristic.value else { return }

        if !transferActive {
            transferActive = true
            log("Receiving audio packet stream...")
        }

        completionTimer?.invalidate()

        // packetNumber += 1
        // totalBytes += data.count
        
        let hex = data.map { String(format: "%02X", $0) }.joined(separator: " ")
        
        // log("Normal Updated Value of Characteristic \(nusTXUUID.uuidString) packet=\(packetNumber) bytes=\(data.count) total_bytes=\(totalBytes) to \(hex)")
        let packetLine = 
            "\(Date().timeIntervalSince1970) " +
            "Updated Value of Characteristic \(nusTXUUID.uuidString) " +
            "bytes=\(data.count) to \(hex)\n"

        if let packetData = packetLine.data(using: .utf8) {
            packetLogHandle?.write(packetData)
        }

        completionTimer = Timer.scheduledTimer(
            withTimeInterval: 1.0,
            repeats: false
        ) { [weak self] _ in
            guard let self = self, self.transferActive else {
                return
            }

            self.transferActive = false
            self.log("Transfer complete. Packet data saved to nrf_ble_packet_log.txt")
        }
    }
}

let logger = PacketLogger()
RunLoop.main.run()