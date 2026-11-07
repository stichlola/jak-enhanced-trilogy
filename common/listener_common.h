#pragma once

/*!
 * @file listener_common.h
 * Common types shared between the compiler and the runtime for the listener connection.
 */

#include <cstdlib>

#include "common/common_types.h"

/*!
 * Header of a DECI2 protocol message
 * NOTE: we've changed this to use 32-bit integers for len/rsvd
 */
struct Deci2Header {
  u32 len;    //! size of data following header
  u32 rsvd;   //! zero, used internally by runtime.
  u16 proto;  //! protocol identification number
  u8 src;     //! identification code of sender
  u8 dst;     //! identification code of recipient
};

/*!
 * Type of message sent to compiler
 */
enum class ListenerMessageKind : u16 {
  MSG_ACK = 0,     //! Acknowledge a compiler message
  MSG_OUTPUT = 1,  //! Send output buffer data
  MSG_PRINT = 2,   //! Send print buffer data
  MSG_INVALID = 24
};

/*!
 * Type of message sent from compiler
 */
enum ListenerToTargetMsgKind : u16 {
  LTT_MSG_POKE = 1,           //! "Poke" the game and have it flush buffers
  LTT_MSG_INSPECT = 5,        //! Inspect an object
  LTT_MSG_PRINT = 6,          //! Print an object
  LTT_MSG_PRINT_SYMBOLS = 7,  //! Print all symbols
  LTT_MSG_RESET = 8,          //! Reset the game
  LTT_MSG_CODE = 9,           //! Send code to patch into the game
  // below here are added
  LTT_MSG_SHUTDOWN = 10  //! Shut down the runtime.
};

/*!
 * The full header of a listener message, including the Deci2Header
 */
struct ListenerMessageHeader {
  Deci2Header deci2_header;  //! The header used for DECI2 communication
  union {
    ListenerMessageKind msg_kind;  //! GOAL Listener message kind
    ListenerToTargetMsgKind ltt_msg_kind;
  };

  u16 u6;        //! Unknown
  u32 msg_size;  //! Size of data after this header
  u64 msg_id;    //! Message ID number, target echoes this back.
};

constexpr int DECI2_PORT = 8112;  // TODO - is this a good choice?

constexpr int DEBUG_SERVER_PORT = 8128;

constexpr u16 DECI2_PROTOCOL = 0xe042;

/*!
 * Jak Enhanced: JAK_ENHANCED_INSTANCE=<n> (1-99) runs a parallel test instance. gk's listener,
 * goalc's nREPL and debug ports all move up by 10*n, so they never meet another game's or another
 * instance's defaults (8112-8114, 8128-8130, 8181). Unset or 0: the usual ports.
 */
inline int enhanced_instance_number() {
  const char* env = std::getenv("JAK_ENHANCED_INSTANCE");
  if (!env) {
    return 0;
  }
  int n = std::atoi(env);
  return (n > 0 && n < 100) ? n : 0;
}

inline int enhanced_port_offset() {
  return 10 * enhanced_instance_number();
}
