import SwiftUI
import AppKit

/// The few shared pieces of the app's look, Glovebox's look from glovebox.vision: the game's art
/// with one glass card per step over it (Cinema.swift), always dark like the art; the orange of
/// the logo's port for progress and done; white pill buttons. Inside the cards it's system fonts
/// and controls, and Liquid Glass on macOS 26 and later.
/// The app's name as it shows: project.yml's PRODUCT_DISPLAY_NAME ("Mitten" on this unofficial branch),
/// so a rename is one line there.
enum AppName {
    static let shown = Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String ?? "Glovebox"
}

enum Theme {
    static let windowSize = CGSize(width: 900, height: 600)
    /// The logo's orange.
    static let accent = Color(red: 1, green: 0.624, blue: 0.18)
    static let link = Color(red: 1, green: 0.765, blue: 0.478)
}

/// Reduce Motion, honored everywhere but in the walkthrough video, which shows what a Mac with
/// the default settings shows (CI's Macs have Reduce Motion on).
enum Motion {
    static func reduced(_ system: Bool) -> Bool { system && !Demo.isTour }
}

/// A colored rounded square with a white symbol, like the icons in System Settings' sidebar.
struct IconTile: View {
    var symbol: String
    var color: Color
    var size: CGFloat = 22

    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.24, style: .continuous)
            .fill(color.gradient)
            .frame(width: size, height: size)
            .overlay {
                Image(systemName: Symbols.available(symbol))
                    .font(.system(size: size * 0.52, weight: .semibold))
                    .foregroundStyle(.white)
            }
    }
}

enum Symbols {
    /// The symbol, or a plain one when this macOS doesn't have it yet.
    static func available(_ name: String, fallback: String = "circle") -> String {
        NSImage(systemSymbolName: name, accessibilityDescription: nil) == nil ? fallback : name
    }
}

/// The mark for a step or a task: done, working, or needs the player.
struct StatusMark: View {
    var state: StepState

    var body: some View {
        Group {
            switch state {
            case .done:
                Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.accent)
                    .transition(.scale.combined(with: .opacity))
            case .working:
                ProgressView().controlSize(.small)
            case .attention:
                Image(systemName: "circle.fill").font(.system(size: 7)).foregroundStyle(.orange)
            case .todo, .waiting:
                EmptyView()
            }
        }
        .frame(width: 18)
        .animation(.spring(duration: 0.35), value: state)
    }
}

/// Where a step, or one task inside it, stands.
enum StepState: Equatable {
    /// Not started; nothing to do yet.
    case todo
    /// Waits for an earlier step (Xcode, usually).
    case waiting
    /// Glovebox, or Apple, or Steam, is working on it.
    case working
    /// The player has something to do here.
    case attention
    case done
}

/// One task inside a step: an icon, what it is, a few words on why or how long, and on the right
/// its one action or its state.
struct TaskRow<Trailing: View>: View {
    var symbol: String
    var color: Color
    var title: String
    var detail: String?
    var state: StepState
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol, color: state == .waiting ? .gray : color, size: 30)
                .opacity(state == .waiting ? 0.55 : 1)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).foregroundStyle(state == .waiting ? .secondary : .primary)
                if let detail {
                    Text(detail).font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .contentTransition(.numericText())
                }
            }
            Spacer(minLength: 12)
            if state == .done {
                StatusMark(state: .done).font(.title3)
            } else {
                trailing
            }
        }
        .padding(.vertical, 4)
        .animation(.spring(duration: 0.35), value: state)
    }
}

extension TaskRow where Trailing == EmptyView {
    init(symbol: String, color: Color, title: String, detail: String? = nil, state: StepState) {
        self.init(symbol: symbol, color: color, title: title, detail: detail, state: state) { EmptyView() }
    }
}

/// Keycaps, for the one shortcut the guide asks for.
struct Keycap: View {
    var key: String
    var body: some View {
        Text(key)
            .font(.system(size: 13, weight: .medium, design: .rounded))
            .frame(minWidth: 24, minHeight: 24)
            .padding(.horizontal, 4)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(.background))
            .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).strokeBorder(.separator))
            .shadow(color: .black.opacity(0.12), radius: 0, y: 1)
    }
}

extension View {
    /// The one thing to do on a page: a white pill with black words, glowing a little so the eye
    /// goes there first, like the buttons on glovebox.vision.
    func primaryButton() -> some View {
        buttonStyle(PillButtonStyle(prominent: true))
    }

    /// A text button inside a card, warm so it reads as clickable over the art.
    func textLink() -> some View {
        buttonStyle(.link).foregroundStyle(Theme.link)
    }

    /// The other choice: the same pill in see-through glass.
    func secondaryButton() -> some View {
        buttonStyle(PillButtonStyle(prominent: false))
    }
}

struct PillButtonStyle: ButtonStyle {
    var prominent: Bool
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            // A spinner in the label draws dark on the white pill.
            .environment(\.colorScheme, prominent ? .light : .dark)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(prominent ? Color(red: 0.04, green: 0.04, blue: 0.05) : .white)
            // A pill never wraps; the links beside it give way instead.
            .lineLimit(1)
            .fixedSize(horizontal: true, vertical: false)
            .padding(.horizontal, 22)
            .frame(minHeight: 40)
            .background {
                if prominent {
                    Capsule().fill(.white.opacity(configuration.isPressed ? 0.78 : 1))
                } else {
                    Capsule().fill(.white.opacity(configuration.isPressed ? 0.24 : 0.14))
                        .overlay(Capsule().strokeBorder(.white.opacity(0.14)))
                }
            }
            .shadow(color: .white.opacity(prominent && enabled ? 0.22 : 0), radius: 14, y: 4)
            .contentShape(Capsule())
            .opacity(enabled ? 1 : 0.45)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}
