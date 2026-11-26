import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 480
    height: 700
    minimumWidth: 440
    minimumHeight: 620
    visible: true
    title: "Steady — 防暈車動態提示"
    color: theme.background
    readonly property string fixedFontFamily: Qt.platform.os === "osx" ? "Menlo"
                                              : Qt.platform.os === "windows" ? "Consolas" : "monospace"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background

    Shortcut {
        sequences: ["Space"]
        context: Qt.ApplicationShortcut
        onActivated: motion.overlayEnabled = !motion.overlayEnabled
    }
    Shortcut {
        sequences: ["C"]
        context: Qt.ApplicationShortcut
        onActivated: motion.calibrate()
    }
    Shortcut {
        sequences: ["Q", StandardKey.Quit]
        context: Qt.ApplicationShortcut
        onActivated: Qt.quit()
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width - 32
            x: 16
            y: 16
            spacing: 14

            // Header bar
            RowLayout {
                Layout.fillWidth: true
                ColumnLayout {
                    spacing: 2
                    Label {
                        text: "Steady"
                        font.pixelSize: 22
                        font.bold: true
                        color: theme.foreground
                    }
                    Label {
                        text: "防暈眩動態覆蓋層（滑鼠穿透）"
                        font.pixelSize: 12
                        color: theme.muted
                    }
                }
                Item { Layout.fillWidth: true }
                Button { text: "離開 (Quit)"; onClicked: Qt.quit() }
                Switch {
                    checked: motion.overlayEnabled
                    text: motion.overlayEnabled ? "開啟" : "關閉"
                    onToggled: motion.overlayEnabled = checked
                }
            }

            // Mode selector
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    Layout.fillWidth: true
                    text: "模擬模式 (Simulation)"
                    highlighted: motion.mode === "sim"
                    onClicked: motion.mode = "sim"
                }
                Button {
                    Layout.fillWidth: true
                    text: "手機 IMU 串流"
                    highlighted: motion.mode === "phone"
                    onClicked: motion.mode = "phone"
                }
            }

            // Live G-Force Radar + Telemetry card
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 132
                radius: 8
                color: theme.surface
                border.color: Qt.rgba(1, 1, 1, 0.08)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 16

                    // 2D Radar
                    Rectangle {
                        width: 104
                        height: 104
                        radius: 52
                        color: theme.background
                        border.color: Qt.rgba(1, 1, 1, 0.14)

                        Rectangle {
                            width: 1; height: parent.height - 16
                            anchors.centerIn: parent
                            color: Qt.rgba(1, 1, 1, 0.08)
                        }
                        Rectangle {
                            height: 1; width: parent.width - 16
                            anchors.centerIn: parent
                            color: Qt.rgba(1, 1, 1, 0.08)
                        }
                        Rectangle {
                            width: 14; height: 14; radius: 7
                            color: theme.accent
                            x: (parent.width - width) / 2 + Math.max(-36, Math.min(36, motion.offsetX * 1.1))
                            y: (parent.height - height) / 2 + Math.max(-36, Math.min(36, motion.offsetY * 1.1))
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: "訊號來源：" + motion.imuSource
                            textFormat: Text.PlainText
                            maximumLineCount: 1
                            font.pixelSize: 13
                            font.bold: true
                            color: theme.foreground
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            visible: server.lockedSource.length > 0
                            text: "鎖定來源：" + server.lockedSource
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            color: theme.muted
                            Layout.fillWidth: true
                        }
                        Label {
                            text: "橫向 G: " + motion.ax.toFixed(2) + " m/s²   縱向 G: " + motion.ay.toFixed(2) + " m/s²"
                            font.pixelSize: 12
                            font.family: root.fixedFontFamily
                            color: theme.muted
                        }
                        Label {
                            text: motion.mode === "phone"
                                  ? ("連線狀態：" + (motion.phoneConnected ? ("已連線 (" + motion.imuHz + " Hz)") : "尚未連線") + " · " + motion.calibrationHint)
                                  : ("螢幕邊緣圓點位移：X " + motion.offsetX.toFixed(1) + " px / Y " + motion.offsetY.toFixed(1) + " px")
                            font.pixelSize: 12
                            color: motion.phoneConnected ? theme.accent : theme.muted
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // Simulation Controls
            RowLayout {
                visible: motion.simulationPaused
                Layout.fillWidth: true
                Label {
                    text: "已閒置 10 分鐘，模擬已暫停。"
                    color: theme.muted
                    Layout.fillWidth: true
                }
                Button { text: "繼續模擬"; onClicked: motion.resumeSimulation() }
            }

            Rectangle {
                visible: motion.mode === "sim"
                Layout.fillWidth: true
                implicitHeight: simCol.implicitHeight + 24
                radius: 8
                color: theme.surface
                border.color: Qt.rgba(1, 1, 1, 0.08)

                ColumnLayout {
                    id: simCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 10

                    Label {
                        text: "模擬情境"
                        font.bold: true
                        font.pixelSize: 13
                        color: theme.foreground
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 6
                        columnSpacing: 8
                        Button {
                            Layout.fillWidth: true
                            text: "市區轉彎＋煞車"
                            highlighted: motion.simPreset === "city"
                            onClicked: motion.simPreset = "city"
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "高速公路巡航"
                            highlighted: motion.simPreset === "highway"
                            onClicked: motion.simPreset = "highway"
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "山路連續彎道"
                            highlighted: motion.simPreset === "mountain"
                            onClicked: motion.simPreset = "mountain"
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "手動點按測試"
                            highlighted: motion.simPreset === "manual"
                            onClicked: motion.simPreset = "manual"
                        }
                    }

                    Label {
                        text: "即時脈衝測試（點了立刻看螢幕左右圓點反應）"
                        font.pixelSize: 12
                        color: theme.muted
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Button { Layout.fillWidth: true; text: "← 左轉"; onClicked: motion.triggerImpulse(-3.2, 0) }
                        Button { Layout.fillWidth: true; text: "↑ 加速"; onClicked: motion.triggerImpulse(0, 3.0) }
                        Button { Layout.fillWidth: true; text: "↓ 煞車"; onClicked: motion.triggerImpulse(0, -3.2) }
                        Button { Layout.fillWidth: true; text: "→ 右轉"; onClicked: motion.triggerImpulse(3.2, 0) }
                    }
                }
            }

            // Phone IMU Controls
            Rectangle {
                visible: motion.mode === "phone"
                Layout.fillWidth: true
                implicitHeight: phoneCol.implicitHeight + 24
                radius: 8
                color: theme.surface
                border.color: Qt.rgba(1, 1, 1, 0.08)

                ColumnLayout {
                    id: phoneCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "手機感應器（支援背景 APK / 網頁）"
                            font.bold: true
                            font.pixelSize: 13
                            color: theme.foreground
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: server.useTailscaleQr ? "改用區網 IP" : "改用 Tailscale"
                            enabled: server.tailscaleUrl.length > 0
                            onClicked: server.useTailscaleQr = !server.useTailscaleQr
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        Rectangle {
                            width: 148
                            height: 148
                            radius: 8
                            color: "#ffffff"
                            visible: server.activeQrUrl.length > 0
                            Image {
                                id: qrImage
                                anchors.fill: parent
                                anchors.margins: 6
                                cache: false
                                source: server.activeQrUrl
                                fillMode: Image.PreserveAspectFit
                                smooth: false
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                visible: server.lanUrl.length > 0 || server.tailscaleUrl.length > 0
                                text: "網址：" + (server.useTailscaleQr && server.tailscaleUrl.length > 0
                                              ? server.tailscaleUrl : server.lanUrl)
                                font.family: root.fixedFontFamily
                                font.pixelSize: 12
                                color: theme.accent
                                wrapMode: Text.WrapAnywhere
                                Layout.fillWidth: true
                            }
                            Label {
                                visible: server.activeQrUrl.length > 0
                                text: "1. 手機掃描 QR，首次瀏覽器提示自簽憑證點「繼續前往」\n2. 點「下載 Android 背景版 APK」安裝，之後可縮小、切 App、關螢幕\n3. 開啟 App 點「開始背景串流」即自動切換成手機感應模式"
                                font.pixelSize: 12
                                color: theme.muted
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Label {
                                visible: server.udpReady
                                text: "UDP :" + server.udpPort + "（支援自動搜尋區網 / Tailscale / HyperIMU）"
                                font.pixelSize: 11
                                color: theme.muted
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            text: "鎖定單一手機來源"
                            checked: server.sourceLockEnabled
                            onToggled: server.sourceLockEnabled = checked
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "釋放來源 (Release)"
                            enabled: server.lockedSource.length > 0
                            onClicked: server.releaseSource()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Button {
                            Layout.fillWidth: true
                            text: "一鍵校正原點 (C)"
                            highlighted: true
                            onClicked: motion.calibrate()
                        }
                        Button {
                            text: "重新偵測 IP"
                            onClicked: server.refreshNetwork()
                        }
                    }
                }
            }

            Label {
                text: server.statusMessage
                textFormat: Text.PlainText
                font.pixelSize: 11
                color: theme.muted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // Visual Tuning Card
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: tuneCol.implicitHeight + 24
                radius: 8
                color: theme.surface
                border.color: Qt.rgba(1, 1, 1, 0.08)

                ColumnLayout {
                    id: tuneCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "視覺與手感調整"; font.bold: true; font.pixelSize: 13; color: theme.foreground }
                        Item { Layout.fillWidth: true }
                        Button { text: "重設預設"; flat: true; onClicked: motion.resetDefaults() }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "圓點數量：" + (motion.dotPairs * 2) + " 顆"; Layout.preferredWidth: 120; font.pixelSize: 12 }
                        Slider { Layout.fillWidth: true; from: 3; to: 6; stepSize: 1; value: motion.dotPairs; onMoved: motion.dotPairs = Math.round(value) }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "圓點大小：" + motion.dotSize.toFixed(0) + " px"; Layout.preferredWidth: 120; font.pixelSize: 12 }
                        Slider { Layout.fillWidth: true; from: 6; to: 22; stepSize: 1; value: motion.dotSize; onMoved: motion.dotSize = value }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "透明度：" + Math.round(motion.opacity * 100) + "%"; Layout.preferredWidth: 120; font.pixelSize: 12 }
                        Slider { Layout.fillWidth: true; from: 0.2; to: 1.0; stepSize: 0.05; value: motion.opacity; onMoved: motion.opacity = value }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "靈敏度：" + motion.sensitivity.toFixed(1) + "x"; Layout.preferredWidth: 120; font.pixelSize: 12 }
                        Slider { Layout.fillWidth: true; from: 0.3; to: 2.5; stepSize: 0.1; value: motion.sensitivity; onMoved: motion.sensitivity = value }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            text: "靜止 3 秒自動淡出"
                            checked: motion.autoHide
                            onToggled: motion.autoHide = checked
                        }
                        CheckBox {
                            text: theme.omarchy ? "使用 Omarchy 主題色" : "使用系統強調色"
                            checked: motion.useAccentColor
                            onToggled: motion.useAccentColor = checked
                        }
                    }
                }
            }

            Label {
                text: "快捷鍵：Space 開關覆蓋層 · C 校正原點 · Q 離開"
                font.pixelSize: 11
                color: theme.muted
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: 12
            }
        }
    }
}
