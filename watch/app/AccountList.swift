// An AI's accounts on the wrist (docs/watch.md, "Switching an AI's account"): a tap on the AI's name opens the list of
// its signed-in accounts, which the Digital Crown scrolls; the account it is on now is marked. A tap on another one
// switches the AI to it, through the phone. What the list holds, and what a tap sends, is decided in the core
// (WatchLink::choose, accountRows); this shows it.

import SwiftUI

// How the list looks. The defaults live here, in the repo, and nowhere else.
enum AccountLook {
    static let nameFont = Font.headline             // the AI's name over the talk screen, which opens the list
    static let rowFont = Font.body
    static let currentMark = "checkmark.circle.fill"
    static let currentColor = Color.green
    static let switchingColor = Color.orange
    static let rowSpacing: CGFloat = 6
}

struct AccountItem: Identifiable, Equatable {
    let account: String
    let shown: String       // the email, else the label, else the name
    let current: Bool
    let switching: Bool
    var id: String { account }
}

struct AccountList: View {
    @ObservedObject var client: Client
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                // Before the phone's first answer the list is drawn empty, not as "none": no message that flips to
                // rows a moment later.
                if client.accounts.isEmpty && client.aisKnown {
                    Text("No signed-in accounts for \(client.shownAi). Sign one in with nexus on your iPhone.")
                        .font(.footnote)
                        .multilineTextAlignment(.center)
                } else {
                    List(client.accounts) { item in
                        Button {
                            client.chooseAccount(item.account)
                            dismiss()
                        } label: {
                            HStack(spacing: AccountLook.rowSpacing) {
                                Text(item.shown)
                                    .font(AccountLook.rowFont)
                                    .lineLimit(2)
                                Spacer(minLength: 0)
                                if item.switching {
                                    Image(systemName: "arrow.triangle.2.circlepath")
                                        .foregroundStyle(AccountLook.switchingColor)
                                        .accessibilityLabel("Switching")
                                } else if item.current {
                                    Image(systemName: AccountLook.currentMark)
                                        .foregroundStyle(AccountLook.currentColor)
                                        .accessibilityLabel("Current account")
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle(client.shownAi)
        }
    }
}
