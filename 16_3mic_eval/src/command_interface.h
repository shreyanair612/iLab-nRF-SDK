/*
 * Text command interface. One command per line, from a terminal or framed in
 * a TP_COMMAND packet by the host tool. Every command is answered with an
 * "ok: ..." or "err: ..." line (TP_ACK / TP_ERROR in binary mode).
 */
#ifndef COMMAND_INTERFACE_H_
#define COMMAND_INTERFACE_H_

void command_interface_init(void);
void command_interface_handle_line(const char *line);

#endif /* COMMAND_INTERFACE_H_ */
