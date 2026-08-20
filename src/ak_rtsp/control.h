#pragma once

/* TCP tuning/debug control server (port 8091).
 *
 * Text protocol (newline-delimited), one client at a time:
 *   LIST                  -> one "name=value" line per known param, then "."
 *   GET <param>           -> "<param>=<value>" or "ERR ..."
 *   SET <param> <value>   -> "OK <param>=<value>" or "ERR ..."
 * Async log lines (tee'd from stdout) are pushed on the same connection
 * prefixed "LOG " whenever they occur, interleaved with command responses —
 * a client tells them apart by the "LOG " prefix, not by strict
 * request/response framing.
 *
 * See docs/serial_console_and_camera_tuning.md for the full
 * parameter list and the PC-side WinUI3 tuning tab that talks this protocol.
 *
 * Call control_start() as the very first thing in main() (before any other
 * printf-producing init) so the stdout tee captures the full boot log, not
 * just ae/night's own logging. control_stop() on shutdown. */

int  control_start(void);
void control_stop(void);
