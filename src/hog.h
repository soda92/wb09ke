/** @file
 *  @brief HoG Service sample
 */

#ifdef __cplusplus
extern "C" {
#endif

struct bt_conn;

void hog_init(void);

void hog_send_report(struct bt_conn *conn, uint8_t report);

#ifdef __cplusplus
}
#endif
