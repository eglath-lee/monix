// monix-qt-ui hello world — tinted with the upstream dark-violet palette
// (app/theme.h: kBg #17151c, kAccent #a78bfa, kText #ece8f0).
import QtQuick

Window {
    id: root
    width: 440
    height: 300
    visible: true
    title: "monix-qt-ui"
    color: "#17151c"

    Column {
        anchors.centerIn: parent
        spacing: 10

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Hello, monix-qt-ui"
            color: "#a78bfa"
            font.pixelSize: 26
            font.bold: true
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Qt Quick on " + Qt.platform + " — no device connected"
            color: "#9e99a8"
            font.pixelSize: 14
        }
    }
}
