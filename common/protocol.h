#ifndef PROTOCOL_H
#define PROTOCOL_H

#define DEFAULT_PORT 8080
#define MAX_LINE     512      /* longest request/response line */
#define MAX_TOKENS   8
#define NAME_LEN     50

/* Error codes sent as:  ERR <code> <text> */
enum {
    ERR_SYNTAX            = 100,
    ERR_NOT_LOGGED_IN     = 101,
    ERR_ALREADY_LOGGED_IN = 102,
    ERR_AUTH              = 103,
    ERR_LOCKED            = 104,
    ERR_FUNDS             = 105,
    ERR_NO_ACCOUNT        = 106,
    ERR_AMOUNT            = 107,
    ERR_SAME_ACCOUNT      = 108,
    ERR_SERVER_FULL       = 109,
    ERR_TIMEOUT           = 110,
    ERR_INVALID_PIN       = 111,
    ERR_INVALID_NAME      = 112,
    ERR_INTERNAL          = 500
};

static inline const char *err_text(int code)
{
    switch (code) {
    case ERR_SYNTAX:            return "Invalid command or arguments";
    case ERR_NOT_LOGGED_IN:     return "Please login first";
    case ERR_ALREADY_LOGGED_IN: return "Already logged in";
    case ERR_AUTH:              return "Invalid account number or PIN";
    case ERR_LOCKED:            return "Account is locked after too many failed attempts";
    case ERR_FUNDS:             return "Insufficient funds";
    case ERR_NO_ACCOUNT:        return "Destination account does not exist";
    case ERR_AMOUNT:            return "Invalid or out-of-range amount";
    case ERR_SAME_ACCOUNT:      return "Cannot transfer to the same account";
    case ERR_SERVER_FULL:       return "Server is full, try again later";
    case ERR_TIMEOUT:           return "Session timed out due to inactivity";
    case ERR_INVALID_PIN:       return "PIN must be 4 to 6 digits";
    case ERR_INVALID_NAME:      return "Invalid name";
    default:                    return "Internal server error";
    }
}
#endif
