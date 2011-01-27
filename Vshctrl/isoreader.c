#include "isoreader.h"
#include <string.h>
#include <pspiofilemgr.h>
#include <psputilsforkernel.h>
#include "printk.h"
#include "utils.h"
#include "systemctrl_private.h"

#define MAX_RETRIES 8
#define MAX_DIR_LEVEL 8
#define CISO_IDX_BUFFER_SIZE 0x200
#define CISO_DEC_BUFFER_SIZE 0x2000
#define ISO_STANDARD_ID "CD001"

typedef struct _CISOHeader {
	u8 magic[4];			/* +00 : 'C','I','S','O'                           */
	u32 header_size;
	u64 total_bytes;	/* +08 : number of original data size              */
	u32 block_size;		/* +10 : number of compressed block size           */
	u8 ver;				/* +14 : version 01                                */
	u8 align;			/* +15 : align of index (offset = index[n]<<align) */
	u8 rsv_06[2];		/* +16 : reserved                                  */
} __attribute__ ((packed)) CISOHeader;

static void *g_ciso_dec_buf = NULL;
static u32 g_CISO_idx_cache[CISO_IDX_BUFFER_SIZE/4];
static int g_ciso_dec_buf_offset = -1;
static CISOHeader g_ciso_h;
static int g_CISO_cur_idx = -1;

static const char * g_filename = NULL;
static char g_sector_buffer[SECTOR_SIZE] __attribute__((aligned(64)));;
static SceUID g_isofd = -1;
static u32 g_total_sectors = 0;
static u32 g_is_compressed = 0;

static Iso9660DirectoryRecord g_root_record;

static inline u32 isoPos2LBA(u32 pos)
{
	return pos / SECTOR_SIZE;
}

static inline u32 isoLBA2Pos(u32 lba, int offset)
{
	return lba * SECTOR_SIZE + offset;
}

static inline u32 isoPos2OffsetInSector(u32 pos)
{
	return pos & (SECTOR_SIZE - 1);
}

static inline u32 isoPos2RestSize(u32 pos)
{
	return SECTOR_SIZE - isoPos2OffsetInSector(pos);
}

static int reOpen(void)
{
	int retries = MAX_RETRIES, fd = -1;

	sceIoClose(g_isofd);

	while(retries -- > 0) {
		fd = sceIoOpen(g_filename, PSP_O_RDONLY, 0777);

		if (fd >= 0) {
			break;
		}

		sceKernelDelayThread(100000);
	}

	if (fd >= 0) {
		g_isofd = fd;
	}

	return fd;
}

static int readRawData(void* addr, u32 size, int offset)
{
	int ret, i;

	for(i=0; i<MAX_RETRIES; ++i) {
		ret = sceIoLseek32(g_isofd, offset, PSP_SEEK_SET);

		if (ret >= 0) {
			break;
		} else {
			printk("%s: got error 0x%08X, reOpening ISO: %s\n", __func__, ret, g_filename);
		}

		sceKernelDelayThread(100000);
	}

	for(i=0; i<MAX_RETRIES; ++i) {
		ret = sceIoRead(g_isofd, addr, size);

		if(ret >= 0) {
			break;
		} else {
			printk("%s: got error 0x%08X, reOpening ISO: %s\n", __func__, ret, g_filename);
		}

		sceKernelDelayThread(100000);
	}

	return ret;
}

static int readSectorCompressed(int sector, void *addr)
{
	int ret;
	int n_sector;
	int offset, next_offset;
	int size;

	n_sector = sector - g_CISO_cur_idx;

	// not within sector idx cache?
	if (g_CISO_cur_idx == -1 || n_sector < 0 || n_sector >= NELEMS(g_CISO_idx_cache)) {
		ret = readRawData(g_CISO_idx_cache, sizeof(g_CISO_idx_cache), (sector << 2) + sizeof(CISOHeader));

		if (ret < 0) {
			return ret;
		}

		g_CISO_cur_idx = sector;
		n_sector = 0;
	}

	offset = (g_CISO_idx_cache[n_sector] & 0x7FFFFFFF) << g_ciso_h.align;

	// is uncompressed data?
	if (g_CISO_idx_cache[n_sector] & 0x80000000) {
		return readRawData(addr, SECTOR_SIZE, offset);
	}

	sector++;
	n_sector = sector - g_CISO_cur_idx;

	if (g_CISO_cur_idx == -1 || n_sector < 0 || n_sector >= NELEMS(g_CISO_idx_cache)) {
		ret = readRawData(g_CISO_idx_cache, sizeof(g_CISO_idx_cache), (sector << 2) + sizeof(CISOHeader));

		if (ret < 0) {
			return ret;
		}

		g_CISO_cur_idx = sector;
		n_sector = 0;
	}

	next_offset = (g_CISO_idx_cache[n_sector] & 0x7FFFFFFF) << g_ciso_h.align;
	size = next_offset - offset;
	
	if (size <= SECTOR_SIZE)
		size = SECTOR_SIZE;

	if (offset < g_ciso_dec_buf_offset || size + offset >= g_ciso_dec_buf_offset + CISO_DEC_BUFFER_SIZE) {
		ret = readRawData(PTR_ALIGN_64(g_ciso_dec_buf), CISO_DEC_BUFFER_SIZE, offset);

		if (ret < 0) {
			g_ciso_dec_buf_offset = 0xFFF00000;

			return ret;
		}

		g_ciso_dec_buf_offset = offset;
	}

	ret = sceKernelDeflateDecompress(addr, SECTOR_SIZE, PTR_ALIGN_64(g_ciso_dec_buf) + offset - g_ciso_dec_buf_offset, 0);

	if (ret >= 0) {
		return SECTOR_SIZE;
	}

	return ret;
}

static int readSector(u32 sector, void *buf)
{
	int ret;
	u32 pos;

	if (g_is_compressed) {
		ret = readSectorCompressed(sector, buf);
	} else {
		pos = isoLBA2Pos(sector, 0);
		ret = readRawData(buf, SECTOR_SIZE, pos);
	}

	return ret;
}

static void normalizeName(char *filename)
{
	char *p;
   
	p = strstr(filename, ";1");

	if (p) {
		*p = '\0';
	}
}

static int findFile(char * file, u32 lba, u32 dir_size, u32 is_dir, Iso9660DirectoryRecord *result_record)
{
	u32 pos;
	int ret;
	Iso9660DirectoryRecord *rec;
	char name[32];
	int re;

	pos = isoLBA2Pos(lba, 0);
	re = lba = 0;

	while ( re < dir_size ) {
		if (isoPos2LBA(pos) != lba) {
			lba = isoPos2LBA(pos);
			ret = readSector(lba, g_sector_buffer);

			if (ret < 0) {
				return ret;
			}
		}

		rec = (Iso9660DirectoryRecord*)&g_sector_buffer[isoPos2OffsetInSector(pos)];

		if(rec->len_dr == 0) {
			u32 remaining;

			remaining = isoPos2RestSize(pos);
			pos += remaining;
			re += remaining;
			continue;
		}
		
		if(rec->len_dr < rec->len_fi + sizeof(*rec)) {
			printk("%s: Corrupt directory record found in %s, LBA %d\n", __func__, g_filename, lba);

			return -12;
		}

		if(rec->len_fi > 32) {
			return -11;
		}

		if(rec->len_fi == 1 && rec->fi == 0) {
			if (0 == strcmp(file, ".")) {
				memcpy(result_record, rec, sizeof(*result_record));

				return 0;
			}
		} else if(rec->len_fi == 1 && rec->fi == 1) {
			if (0 == strcmp(file, "..")) {
				// didn't support ..
				return -19;
			}
		} else {
			memset(name, 0, sizeof(name));
			memcpy(name, &rec->fi, rec->len_fi);
			normalizeName(name);

			if (0 == strcmp(name, file)) {
				if (is_dir) {
					if(!rec->fileFlags & ISO9660_FILEFLAGS_DIR) {
						return -14;
					}
				}

				memcpy(result_record, rec, sizeof(*result_record));

				return 0;
			}
		}

		pos += rec->len_dr;
		re += rec->len_dr;
	}

	return -18;
}

static int findPath(const char *path, Iso9660DirectoryRecord *result_record)
{
	int level = 0, ret;
	const char *cur_path, *next;
	u32 lba, dir_size;
	char cur_dir[32];

	if (result_record == NULL) {
		return -17;
	}

	memset(result_record, 0, sizeof(*result_record));
	lba = g_root_record.lsbStart;
	dir_size = g_root_record.lsbDataLength;

	cur_path = path;

	while(*cur_path == '/') {
		cur_path++;
	}

	next = strchr(cur_path, '/');

	while (next != NULL) {
		if (next-cur_path >= sizeof(cur_dir)) {
			return -15;
		}

		memset(cur_dir, 0, sizeof(cur_dir));
		strncpy(cur_dir, cur_path, next-cur_path);
		cur_dir[next-cur_path] = '\0';

		if (0 == strcmp(cur_dir, ".")) {
		} else if (0 == strcmp(cur_dir, "..")) {
			level--;
		} else {
			level++;
		}

		if(level > MAX_DIR_LEVEL) {
			return -16;
		}

		ret = findFile(cur_dir, lba, dir_size, 1, result_record);

		if (ret < 0) {
			return ret;
		}

		lba = result_record->lsbStart;
		dir_size = result_record->lsbDataLength;

		cur_path=next+1;

		// skip unwant path separator
		while(*cur_path == '/') {
			cur_path++;
		}
		
		next = strchr(cur_path, '/');
	}

	ret = findFile(cur_path, lba, dir_size, 0, result_record);

	return ret;
}

int isoOpen(const char *path)
{
	int ret;

	if (g_isofd >= 0) {
		isoClose();
	}

	g_filename = path;

	if (reOpen() < 0) {
		printk("%s: open failed %s -> 0x%08X\n", __func__, g_filename, g_isofd);
		ret = -2;
		goto error;
	}

	sceIoLseek32(g_isofd, 0, PSP_SEEK_SET);
	memset(&g_ciso_h, 0, sizeof(g_ciso_h));
	ret = sceIoRead(g_isofd, &g_ciso_h, sizeof(g_ciso_h));

	if (ret != sizeof(g_ciso_h)) {
		ret = -9;
		goto error;
	}

	if (*(u32*)g_ciso_h.magic == 0x4F534943 && g_ciso_h.block_size == SECTOR_SIZE) {
		g_is_compressed = 1;
	} else {
		g_is_compressed = 0;
	}

	if (g_is_compressed) {
		g_total_sectors = g_ciso_h.total_bytes / g_ciso_h.block_size;
		g_CISO_cur_idx = -1;

		if (g_ciso_dec_buf == NULL) {
			g_ciso_dec_buf = oe_malloc(CISO_DEC_BUFFER_SIZE + 64);

			if (g_ciso_dec_buf == NULL) {
				printk("oe_malloc -> 0x%08x\n", (u32)g_ciso_dec_buf);
				ret = -6;
				goto error;
			}
		}

		memset(g_CISO_idx_cache, 0, sizeof(g_CISO_idx_cache));
		g_ciso_dec_buf_offset = -1;
		g_CISO_cur_idx = -1;
	} else {
		g_total_sectors = isoGetSize();
	}

	ret = readSector(16, g_sector_buffer);

	if (ret < 0) {
		ret = -7;
		goto error;
	}

	if (memcmp(&g_sector_buffer[1], ISO_STANDARD_ID, sizeof(ISO_STANDARD_ID)-1)) {
		printk("%s: vol descriptor not found\n", __func__);
		ret = -10;

		goto error;
	}

	memcpy(&g_root_record, &g_sector_buffer[0x9C], sizeof(g_root_record));

	return 0;

error:
	if (g_isofd >= 0) {
		isoClose();
	}

	return ret;
}

int isoGetSize(void)
{
	int ret, size;

	ret = sceIoLseek32(g_isofd, 0, PSP_SEEK_CUR);
	size = sceIoLseek32(g_isofd, 0, PSP_SEEK_END);

	sceIoLseek(g_isofd, ret, PSP_SEEK_SET);

	return isoPos2LBA(size);
}

void isoClose(void)
{
	sceIoClose(g_isofd);
	g_isofd = -1;
	g_filename = NULL;

	if (g_ciso_dec_buf != NULL) {
		oe_free(g_ciso_dec_buf);
		g_ciso_dec_buf = NULL;
	}
}

int isoGetFileInfo(char * path, u32 *filesize, u32 *lba)
{
	u32 filenamepos = 0;
	u8 tempdata[12];
	int ret;
	Iso9660DirectoryRecord rec;

	ret = findPath(path, &rec);

	if (ret < 0) {
		return ret;
	}

	*lba = rec.lsbStart;

	if (filesize != NULL) {
		*filesize = rec.lsbDataLength;
	}

	return 0;
}

int isoRead(void *buffer, u32 lba, int offset, u32 size)
{
	u32 remaining;
	u32 pos, copied;
	u32 re;
	int ret;

	remaining = size;
	pos = isoLBA2Pos(lba, offset);
	copied = 0;

	while(remaining > 0) {
		ret = readSector(isoPos2LBA(pos), g_sector_buffer);

		if (ret < 0) {
			break;
		}

		re = MIN(isoPos2RestSize(pos), remaining);
		memcpy(buffer+copied, g_sector_buffer+isoPos2OffsetInSector(pos), re);
		remaining -= re;
		pos += re;
		copied += re;
	}

	return copied;
}