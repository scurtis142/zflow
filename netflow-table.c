#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

#include "netflow-table.h"
#include "hash.c"

sem_t n_table_mutex;

struct netflow_table* netflow_table_init (void) {
   struct netflow_table *table;

   printf("Initialising NetFlow Table\n");
   sem_init (&n_table_mutex, 0, 1);
   table            = malloc (sizeof (struct netflow_table));
   if(table == NULL)
      exit(1);
   table->array     = malloc (sizeof (hashBucket_t *) * TABLE_INITIAL_SIZE); /* allocates each one in the insert as needed */
   bzero (table->array, sizeof (hashBucket_t *) * TABLE_INITIAL_SIZE);
   table->n_entries = TABLE_INITIAL_SIZE;

   return table;
}


/* Returns 1 if success 0 if failure */
int get_netflow_k_v (const unsigned char *_p, int len, netflow_key_t *key, netflow_value_t *value) {
   const unsigned char *p = _p;

   p += 12;//skip to type

   if (0x0800 == ntohs(*(uint16_t *)p)) {//if type == ipv4
      p += 4;//pointing to total length;
      /* if (len - 14 == ntohs(*(uint16_t *)p)) {//check length packet - eth header = length of ipv4 packet */
      value->bytes = ntohs(*(uint16_t *)p);
      value->packets = 1;
      p += 7;//pointing at protocol
      if (0x06 == *p) {//if protocol = tcp
         key->proto = *p;
         p+=3;//pointing to src
         key->ip_src = *(uint32_t *)p;
         p+=4;
         key->ip_dst = *(uint32_t *)p;
         p+=4;
         key->port_src = ntohs(*(uint16_t *)p);
         p+=2;
         key->port_dst = ntohs(*(uint16_t *)p);
         // here we just want to return non-negative to indicate success,
         // but im also lazy to get rid of the len argument so we are
         // going to return that so it can be used.
         return len+1;

      } else {
         printf ("protocol not tcp, it is %d\n", *p);
      }
      /* } else { */
      /*    printf("length dosen't match\n"); */
      /* } */
   }else{
      printf("type not ipv4\n");
   }

   return 0;
}

/* void netflow_k_v_from_ring (const char *_p, int len, struct netmap_ring *ring, int cur) { */

/* } */


void netflow_table_insert (struct netflow_table *table, netflow_key_t *key, netflow_value_t *val) {
   hashBucket_t *bucket = NULL;
   hashBucket_t *new_bucket = NULL;
   hashBucket_t *previous_pointer = NULL;
   uint32_t idx = 0;
   uint8_t notfound = 0;
   uint8_t updated = 0;

   idx = crc32c_1word (key->proto, idx);
   idx = crc32c_1word (key->ip_src, idx);
   idx = crc32c_1word (key->ip_dst, idx);
   idx = crc32c_1word (key->port_src, idx);
   idx = crc32c_1word (key->port_dst, idx);
   idx = idx % table->n_entries;

   sem_wait (&n_table_mutex);

   /* there might be some weird c thing here cause it was declared as a
      pointer (**) but accessed as an array so idk */
   bucket = table->array[idx];
   previous_pointer = bucket;

   /* MAKE SURE ARRAY IS INITIALISED IN INIT FUNCTION */
   while (bucket != NULL) {
      if (bucket->ip_src == key->ip_src &&
            bucket->ip_dst == key->ip_dst &&
            bucket->proto == key->proto &&
            bucket->port_src == key->port_src &&
            bucket->port_dst == key->port_dst) {


         bucket->bytesSent += val->bytes;
         bucket->pktSent += val->packets;
         updated = 1;
         break;
      }
      notfound = 1;
      previous_pointer = bucket;
      bucket = bucket->next;
   }

   /* First time using this bucket */
   if (!updated) {
      new_bucket = malloc (sizeof (hashBucket_t));
      new_bucket->proto     = key->proto;
      new_bucket->ip_src    = key->ip_src;
      new_bucket->ip_dst    = key->ip_dst;
      new_bucket->port_src  = key->port_src;
      new_bucket->port_dst  = key->port_dst;
      new_bucket->bytesSent = val->bytes;
      new_bucket->pktSent   = val->packets;
      new_bucket->next = NULL;

      if (notfound)
         previous_pointer->next = new_bucket;
      else
         table->array[idx] = new_bucket;
   }
   sem_post(&n_table_mutex);
}


/* void netflow_table_free (struct netflow_table *table) { */

/* } */


void netflow_table_print (struct netflow_table *table) {
   hashBucket_t *bucket;
   struct in_addr src_addr;
   struct in_addr dst_addr;

   printf ("\nPrinting Flow Table\n");
   for (unsigned int i = 0; i < table->n_entries; i++) {
		bucket = table->array[i];
		while (bucket != NULL) {
         src_addr.s_addr = ntohl(bucket->ip_src);
         dst_addr.s_addr = ntohl(bucket->ip_dst);
         printf ("IP Source:     %s\n", inet_ntoa(src_addr));
         printf ("IP Destin:     %s\n", inet_ntoa(dst_addr));
         printf ("Port Source:   %d\n", bucket->port_src);
         printf ("Port Destin:   %d\n", bucket->port_dst);
         printf ("Protocol:      %d\n", bucket->proto);
         printf ("Packets:       %ld\n", bucket->pktSent);
         printf ("Bytes:         %ld\n", bucket->bytesSent);
         printf("\n");

         bucket = bucket->next;
		}
	}
}


void netflow_table_print_stats (struct netflow_table *table) {

   hashBucket_t *bucket;
   uint64_t total_bytes = 0;
   uint64_t total_pkts = 0;
   uint64_t non_null_entries = 0;

   printf ("\nPrinting Flow Table Statistics\n");
   for (unsigned int i = 0; i < table->n_entries; i++) {
		bucket = table->array[i];
		while (bucket != NULL) {
         total_bytes += bucket->bytesSent;
         total_pkts  += bucket->pktSent;
         non_null_entries++;
         bucket = bucket->next;
		}
	}

   printf ("Entries     = %lu\n", non_null_entries);
   printf ("total bytes = %lu\n", total_bytes);
   printf ("total pkts  = %lu\n", total_pkts);
   printf("\n");
}


void netflow_table_export_to_file (struct netflow_table *table, const char *filename) {
   int buf_size = EXPORT_BUF_INITAL_SIZE;
   int fd;
   int snp_res;
   size_t buf_end_offset = 0;
   char *buf;
   const char *tmpfile = "/tmp/netflow-export-tmp.csv";
   hashBucket_t *bucket;
   struct in_addr src_addr;
   struct in_addr dst_addr;
   char src_ip_str[16];
   char dst_ip_str[16];

   if ((buf = malloc (sizeof (char) * buf_size)) == NULL) {
      printf ("malloc failed with %s\n", strerror (errno));
      exit (1);
   }

   sem_wait(&n_table_mutex);

   for (unsigned int i = 0; i < table->n_entries; i++) {
		bucket = table->array[i];
		while (bucket != NULL) {
         /* Free space needed in buffer is maximum number of digits needed to represent
          * an entry which is 91(including null byte) */
         if ((buf_size - buf_end_offset) <= 91) {
            if ((buf = realloc (buf, buf_size * 2)) == NULL) {
               printf ("realloc failed with error %s\n", strerror (errno));
               exit (1);
            } else {
               buf_size *= 2;
            }
         }

         /* Need to copy the string here rather than use directly in the sprintf
            because inet_ntoa return a static buffer that get written over on 
            subsequent calls */
         src_addr.s_addr = bucket->ip_src;
         dst_addr.s_addr = bucket->ip_dst;
         strcpy(src_ip_str, inet_ntoa(src_addr));
         strcpy(dst_ip_str, inet_ntoa(dst_addr));

         snp_res = snprintf ((buf + buf_end_offset), 91, "%s,%s,%d,%d,%d,%lu,%lu\n",
               src_ip_str,
               dst_ip_str,
               bucket->port_src,
               bucket->port_dst,
               bucket->proto,
               bucket->bytesSent,
               bucket->pktSent);

         if (snp_res < 0) {
            printf ("sprintf failed with %s\n", strerror (errno));
            exit (1);
         }
         buf_end_offset += snp_res;
         bucket = bucket->next;
		}
	}

   sem_post(&n_table_mutex);

   /* More effeciant to just do a single write */
   if ((fd = open (tmpfile, O_WRONLY | O_CREAT, S_IRWXU | S_IRWXO)) < 0) { /* Returns non-negative integer on success */
      printf ("open failed with error %s\n", strerror (errno));
      exit (1);
   }

   if((int)buf_end_offset != write (fd, buf, buf_end_offset)) {
      printf ("write didn't return expected number of bytes\n");
      exit (1);
   }

   if (close (fd) != 0) { /* Returns 0 on success */
      printf ("close failed with error %s\n", strerror (errno));
      exit (1);
   }

   rename (tmpfile, filename);
   return;
}
