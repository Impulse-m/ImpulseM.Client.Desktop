"""Layer 232 methods that the ImpulseM server cannot carry.

These methods are omitted from the desktop proto mapping, so a call to one of them
fails locally with CLIENT_METHOD_NOT_MAPPED. This is a mapping omission, not a UI
hide. The feature UI for these groups is decided separately.

The groups are: the TON wallet (wallet.*, toncenter.*, payments.getCurrencyRates),
TON Connect (wallet.tonConnect*), onramp, share themes, no-paid-messages tokens,
star-gift deposit, withdraw, display and delete, and the star-gift withdrawal URL.
"""
from typing import FrozenSet, Iterable, List

WALLET_METHODS: FrozenSet[str] = frozenset({
    "wallet.getState",
    "wallet.getUserAddresses",
    "wallet.getTransactions",
    "wallet.getTransactionsByIDs",
    "wallet.getTransactionsByMsgHash",
    "wallet.exportSecretPhrase",
    "wallet.fetchEncryptedSecretPhrasePart",
    "wallet.replaceWallet",
    "wallet.disableBackup",
    "wallet.getBackupHolderDcs",
    "wallet.enableBackup",
    "wallet.getProofChallenge",
    "wallet.sendTransfer",
    "wallet.getGaslessInfo",
    "wallet.getExistingWaltBalance",
    "wallet.getNfts",
    "wallet.setCardStickers",
})
TON_CONNECT_METHODS: FrozenSet[str] = frozenset({
    "wallet.tonConnectCreateSession",
    "wallet.tonConnectRegisterKey",
    "wallet.tonConnectSubmitConnectResult",
    "wallet.tonConnectGetPending",
    "wallet.tonConnectClaimRequest",
    "wallet.tonConnectSubmitResponse",
    "wallet.tonConnectNextEventId",
    "wallet.tonConnectCloseSession",
    "wallet.tonConnectGetSessions",
})
TON_CENTER_METHODS: FrozenSet[str] = frozenset({
    "toncenter.performApiRequest",
    "toncenter.getStreamingUrl",
})
CURRENCY_METHODS: FrozenSet[str] = frozenset({
    "payments.getCurrencyRates",
})
ONRAMP_METHODS: FrozenSet[str] = frozenset({
    "payments.getOnrampProviders",
    "payments.getOnrampBaseCurrencies",
    "payments.getOnrampAvailability",
    "payments.getOnrampLimits",
    "payments.getOnrampQuote",
    "payments.createOnrampSession",
})
SHARE_THEME_METHODS: FrozenSet[str] = frozenset({
    "messages.setShareTheme",
    "messages.getShareThemeBackdrops",
})
NO_PAID_MESSAGES_TOKEN_METHODS: FrozenSet[str] = frozenset({
    "contacts.exportNoPaidMessagesToken",
    "contacts.importNoPaidMessagesToken",
})
STAR_GIFT_METHODS: FrozenSet[str] = frozenset({
    "payments.depositStarGift",
    "payments.withdrawStarGift",
    "payments.displayStarGift",
    "payments.deleteStarGift",
    "payments.getStarGiftWithdrawalUrl",
})
HIDDEN_METHODS: FrozenSet[str] = (
    WALLET_METHODS
    | TON_CONNECT_METHODS
    | TON_CENTER_METHODS
    | CURRENCY_METHODS
    | ONRAMP_METHODS
    | SHARE_THEME_METHODS
    | NO_PAID_MESSAGES_TOKEN_METHODS
    | STAR_GIFT_METHODS
)


def is_hidden_method(name: str) -> bool:
    return name in HIDDEN_METHODS


def hidden_methods_in(names: Iterable[str]) -> List[str]:
    return sorted(name for name in names if name in HIDDEN_METHODS)
