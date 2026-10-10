/** @file ip_ban_table.h
 *  @brief In-memory concurrent IP ban table with read-write locks and lazy TTL expiration.
 */
#ifndef AIGATE_IP_BAN_TABLE_H
#define AIGATE_IP_BAN_TABLE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IP_BAN_BUCKETS 4096

typedef struct ip_ban_info {
    char     ip[48];
    int64_t  expires_at_sec;
    char     reason[64];
    uint32_t hit_count;
} ip_ban_info_t;

typedef struct ip_ban_entry {
    ip_ban_info_t        info;
    struct ip_ban_entry* next;
} ip_ban_entry_t;

typedef struct ip_ban_table {
    ip_ban_entry_t*  buckets[IP_BAN_BUCKETS];
    pthread_rwlock_t rwlock;
    size_t           count;
} ip_ban_table_t;

/**
 * @brief Allocate and initialize an in-memory IP ban table.
 * @return Pointer to table or NULL on allocation error.
 */
ip_ban_table_t* ip_ban_table_create(void);

/**
 * @brief Destroy an IP ban table and release all allocated entries.
 * @param tbl Table to destroy.
 */
void ip_ban_table_destroy(ip_ban_table_t* tbl);

/**
 * @brief Check whether an IP is currently banned and not expired.
 * Increments hit_count if banned.
 * @param tbl Table pointer.
 * @param ip Client IP string.
 * @param out_reason Optional buffer to receive ban reason.
 * @param cap Capacity of out_reason buffer.
 * @return true if banned and unexpired; false otherwise.
 */
bool ip_ban_table_is_banned(ip_ban_table_t* tbl, const char* ip, char* out_reason, size_t cap);

/**
 * @brief Add or update an IP in the ban table with a TTL.
 * @param tbl Table pointer.
 * @param ip Client IP string.
 * @param ttl_sec Duration in seconds from now; if <= 0 defaults to 3600.
 * @param reason Reason description string.
 * @return 0 on success, -1 on error.
 */
int ip_ban_table_ban(ip_ban_table_t* tbl, const char* ip, int64_t ttl_sec, const char* reason);

/**
 * @brief Remove an IP from the ban table.
 * @param tbl Table pointer.
 * @param ip Client IP string.
 * @return 0 on success, -1 if not found or error.
 */
int ip_ban_table_unban(ip_ban_table_t* tbl, const char* ip);

/**
 * @brief List all active, unexpired IP bans.
 * @param tbl Table pointer.
 * @param out_arr Array to receive ban entries.
 * @param max_count Maximum number of entries to write to out_arr.
 * @return Number of unexpired entries written to out_arr.
 */
size_t ip_ban_table_list(ip_ban_table_t* tbl, ip_ban_info_t* out_arr, size_t max_count);

#endif /* AIGATE_IP_BAN_TABLE_H */
