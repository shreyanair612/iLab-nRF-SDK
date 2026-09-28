/*
 * Line-oriented command interface. Lines arrive from the transport either as
 * plain text typed into a terminal or wrapped in TP_COMMAND packets by the
 * host tool; both reach command_interface_handle_line().
 *
 * Replies go out as TP_ACK text ("ok ..." / "err ...") so the host can match
 * them to the command it sent.
 */
#ifndef COMMAND_INTERFACE_H_
#define COMMAND_INTERFACE_H_

void command_interface_init(void);
void command_interface_handle_line(const char *line);

#endif /* COMMAND_INTERFACE_H_ */
