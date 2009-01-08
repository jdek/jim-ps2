//---------------------------------------------------------------------------
//File name:    fat_driver.c
//---------------------------------------------------------------------------
#include <stdio.h>
#include <sysclib.h>
//#include <sys/stat.h>

#include <thbase.h>
#include <errno.h>
#include <sysmem.h>
#define malloc(a)       AllocSysMemory(0,(a), NULL)
#define free(a)         FreeSysMemory((a))

#include "usbhd_common.h"
#include "scache.h"
#include "fat_driver.h"
//#include "fat_write.h"
#include "mass_stor.h"

//#define DEBUG  //comment out this line when not debugging

#include "mass_debug.h"

//#define DISK_INIT(d,b) scache_init((d), (b))
//#define DISK_CLOSE     scache_close
//#define DISK_KILL      scache_kill  //dlanor: added for disconnection events (no flush)
#define READ_SECTOR(d, a, b)	scache_readSector((d)->cache, (a), (void **)&b)
//#define FLUSH_SECTORS		scache_flushSectors

static fat_driver* g_fatd[NUM_DEVICES];

//---------------------------------------------------------------------------
int InitFAT()
{
    int i;
	int ret = 0;
    for (i = 0; i < NUM_DEVICES; ++i)
        g_fatd[i] = NULL;
    return ret;
}

//---------------------------------------------------------------------------
int getI32(unsigned char* buf)
{
	return (buf[0]  + (buf[1] <<8) + (buf[2] << 16) + (buf[3] << 24));
}

//---------------------------------------------------------------------------
int getI32_2(unsigned char* buf1, unsigned char* buf2)
{
	return (buf1[0]  + (buf1[1] <<8) + (buf2[0] << 16) + (buf2[1] << 24));
}

//---------------------------------------------------------------------------
int getI16(unsigned char* buf)
{
	return (buf[0] + (buf[1] <<8) );
}

//---------------------------------------------------------------------------
int strEqual(unsigned char *s1, unsigned char* s2) {
    unsigned char u1, u2;
    for (;;) {
		u1 = *s1++;
		u2 = *s2++;
		if (u1 >64 && u1 < 91)  u1+=32;
		if (u2 >64 && u2 < 91)  u2+=32;

		if (u1 != u2) {
			return -1;
		}
		if (u1 == '\0') {
		    return 0;
		}
    }
}

//---------------------------------------------------------------------------
inline unsigned int fat_cluster2sector1216(fat_bpb* partBpb, unsigned int cluster)
{
	return  partBpb->rootDirStart + (partBpb->rootSize / (partBpb->sectorSize>>5))+ (partBpb->clusterSize * (cluster-2));
                           //modified by Hermes    ^ this work :)
}

//---------------------------------------------------------------------------
inline unsigned int fat_cluster2sector32(fat_bpb* partBpb, unsigned int cluster)
{
	return  partBpb->rootDirStart + (partBpb->clusterSize * (cluster-2));
}

//---------------------------------------------------------------------------
inline unsigned int fat_cluster2sector(fat_bpb* partBpb, unsigned int cluster)
{
	switch(partBpb->fatType)
	{
		case FAT32: return fat_cluster2sector32(partBpb, cluster);
		default:    return fat_cluster2sector1216(partBpb, cluster);
	}
}

//---------------------------------------------------------------------------
inline void fat_getPartitionRecord(part_raw_record* raw, part_record* rec)
{
	rec->sid = raw->sid;
	rec->start = getI32(raw->startLBA);
	rec->count = getI32(raw->size);
}

/*

   0x321, 0xABC

     byte| byte| byte|
   +--+--+--+--+--+--+
   |2 |1 |C |3 |A |B |
   +--+--+--+--+--+--+

*/

//---------------------------------------------------------------------------
unsigned int fat_getClusterRecord12(unsigned char* buf, int type) {
	if (type) { //1
		return ((buf[1]<< 4) + (buf[0] >>4));
	} else { // 0
		return (((buf[1] & 0x0F) << 8) + buf[0]);
	}
}

//---------------------------------------------------------------------------
// Get Cluster chain into <buf> buffer
// returns:
// 0    :if buf is full (bufSize entries) and more chain entries exist
// 1-n  :number of filled entries of the buf
// -1   :error
//---------------------------------------------------------------------------
//for fat12
/* fat12 cluster records can overlap the edge of the sector so we need to detect and maintain
   these cases
*/
int fat_getClusterChain12(fat_driver* fatd, unsigned int cluster, unsigned int* buf, int bufSize, int start) {
	int ret;
	int i;
	int recordOffset;
	int sectorSpan;
	int fatSector;
	int cont;
	int lastFatSector;
	unsigned char xbuf[4];
	unsigned char* sbuf = NULL; //sector buffer

	cont = 1;
	lastFatSector = -1;
	i = 0;
	if (start) {
		buf[i] = cluster; //strore first cluster
		i++;
	}
	while(i < bufSize && cont) {
		recordOffset = (cluster * 3) / 2; //offset of the cluster record (in bytes) from the FAT start
		fatSector = recordOffset / fatd->partBpb.sectorSize;
		sectorSpan = 0;
		if ((recordOffset % fatd->partBpb.sectorSize) == (fatd->partBpb.sectorSize - 1)) {
			sectorSpan = 1;
		}
		if (lastFatSector !=  fatSector || sectorSpan) {
				ret = READ_SECTOR(fatd->dev, fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector, sbuf);
				if (ret < 0) {
					printf("FAT driver:Read fat12 sector failed! sector=%i! \n", fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector );
					return -EIO;
				}
				lastFatSector = fatSector;

				if (sectorSpan) {
					xbuf[0] = sbuf[fatd->partBpb.sectorSize - 2];
					xbuf[1] = sbuf[fatd->partBpb.sectorSize - 1];
					ret = READ_SECTOR(fatd->dev, fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector + 1, sbuf);
					if (ret < 0) {
						printf("FAT driver:Read fat12 sector failed sector=%i! \n", fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector + 1);
						return -EIO;
					}
					xbuf[2] = sbuf[0];
					xbuf[3] = sbuf[1];
				}
		}
		if (sectorSpan) { // use xbuf as source buffer
			cluster = fat_getClusterRecord12(xbuf + (recordOffset % fatd->partBpb.sectorSize) - (fatd->partBpb.sectorSize-2), cluster % 2);
		} else { // use sector buffer as source buffer
			cluster = fat_getClusterRecord12(sbuf + (recordOffset % fatd->partBpb.sectorSize), cluster % 2);
		}

		if (cluster >= 0xFF8) {
			cont = 0; //continue = false
		} else {
			buf[i] = cluster;
			i++;
		}
	}

	if (cont) { return 0; }

	return i;
}


//---------------------------------------------------------------------------
//for fat16
int fat_getClusterChain16(fat_driver* fatd, unsigned int cluster, unsigned int* buf, int bufSize, int start) {
	int ret;
	int i;
	int indexCount;
	int fatSector;
	int cont;
	int lastFatSector;
	unsigned char* sbuf = NULL; //sector buffer

	cont = 1;
	indexCount = fatd->partBpb.sectorSize / 2; //FAT16->2, FAT32->4
	lastFatSector = -1;
	i = 0;
	if (start) {
		buf[i] = cluster; //strore first cluster
		i++;
	}
	while(i < bufSize && cont) {
		fatSector = cluster / indexCount;
		if (lastFatSector !=  fatSector) {
				ret = READ_SECTOR(fatd->dev, fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector,  sbuf);
				if (ret < 0) {
					printf("FAT driver:Read fat16 sector failed! sector=%i! \n", fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector );
					return -EIO;
				}

				lastFatSector = fatSector;
		}
		cluster = getI16(sbuf + ((cluster % indexCount) * 2));
		if (cluster >= 0xFFF8) {
			cont = 0; //continue = false
		} else {
			buf[i] = cluster;
			i++;
		}
	}
	if (cont) {
		return 0;
	} else {
		return i;
	}
}

//---------------------------------------------------------------------------
//for fat32
int fat_getClusterChain32(fat_driver* fatd, unsigned int cluster, unsigned int* buf, int bufSize, int start) {
	int ret;
	int i;
	int indexCount;
	int fatSector;
	int cont;
	int lastFatSector;
	unsigned char* sbuf = NULL; //sector buffer

	cont = 1;
	indexCount = fatd->partBpb.sectorSize / 4; //FAT16->2, FAT32->4
	lastFatSector = -1;
	i = 0;
	if (start) {
		buf[i] = cluster; //store first cluster
		i++;
	}
	while(i < bufSize && cont) {
		fatSector = cluster / indexCount;
		if (lastFatSector !=  fatSector) {
				ret = READ_SECTOR(fatd->dev, fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector,  sbuf);
				if (ret < 0) {
					printf("FAT driver: Read fat32 sector failed sector=%i! \n", fatd->partBpb.partStart + fatd->partBpb.resSectors + fatSector );
					return -EIO;
				}

				lastFatSector = fatSector;
		}
		cluster = getI32(sbuf + ((cluster % indexCount) * 4));
		if ((cluster & 0xFFFFFFF) >= 0xFFFFFF8) {
			cont = 0; //continue = false
		} else {
			buf[i] = cluster & 0xFFFFFFF;
			i++;
		}
	}
	if (cont) {
		return 0;
	} else {
		return i;
	}
}

//---------------------------------------------------------------------------
int fat_getClusterChain(fat_driver* fatd, unsigned int cluster, unsigned int* buf, int bufSize, int start) {

	if (cluster == fatd->lastChainCluster) {
		return fatd->lastChainResult;
	}

	switch (fatd->partBpb.fatType) {
		case FAT12: fatd->lastChainResult = fat_getClusterChain12(fatd, cluster, buf, bufSize, start); break;
		case FAT16: fatd->lastChainResult = fat_getClusterChain16(fatd, cluster, buf, bufSize, start); break;
		case FAT32: fatd->lastChainResult = fat_getClusterChain32(fatd, cluster, buf, bufSize, start); break;
	}
	fatd->lastChainCluster = cluster;
	return fatd->lastChainResult;
}

//---------------------------------------------------------------------------
void fat_invalidateLastChainResult(fat_driver* fatd)
{
	fatd->lastChainCluster  = 0;
}

//---------------------------------------------------------------------------
int fat_getPartitionTable(mass_dev* dev, fat_part* part)
{
    part_raw_record* part_raw;
    int              i;
    int              ret;
	unsigned char* sbuf = NULL; //sector buffer

    ret = READ_SECTOR(dev, 0, sbuf);  // read sector 0 - Disk MBR or boot sector
    if ( ret < 0 )
    {
        printf ( "FAT driver: Read sector 0 failed!\n" );
        return -EIO;
    }

    /* read 4 partition records */
    for ( i = 0; i < 4; i++)
    {
        part_raw = ( part_raw_record* )(  sbuf + 0x01BE + ( i * 16 )  );

        fat_getPartitionRecord ( part_raw, &part -> record[ i ] );
    }
    return 0;
}

//---------------------------------------------------------------------------
void fat_determineFatType(fat_bpb* partBpb) {
	int sector;
	int clusterCount;

	//get sector of cluster 0
	sector = fat_cluster2sector(partBpb, 0);
	//remove partition start sector to get BR+FAT+ROOT_DIR sector count
	sector -= partBpb->partStart;
	sector = partBpb->sectorCount - sector;
	clusterCount = sector / partBpb->clusterSize;
	//printf("Data cluster count = %i \n", clusterCount);

	if (clusterCount < 4085) {
		partBpb->fatType = FAT12;
	} else
	if (clusterCount < 65525) {
		partBpb->fatType = FAT16;
	} else {
		partBpb->fatType = FAT32;
	}
}

//---------------------------------------------------------------------------
int fat_getPartitionBootSector(mass_dev* dev, part_record* part_rec, fat_bpb* partBpb) {
	fat_raw_bpb* bpb_raw; //fat16, fat12
	fat32_raw_bpb* bpb32_raw; //fat32
	int ret;
	unsigned char* sbuf = NULL; //sector buffer

	ret = READ_SECTOR(dev, part_rec->start, sbuf); //read partition boot sector (first sector on partition)

	if (ret < 0) {
		printf("FAT driver: Read partition boot sector failed sector=%i! \n", part_rec->start);
		return -EIO;
	}

	bpb_raw = (fat_raw_bpb*) sbuf;
	bpb32_raw = (fat32_raw_bpb*) sbuf;

	//set fat common properties
	partBpb->sectorSize	= getI16(bpb_raw->sectorSize);
	partBpb->clusterSize = bpb_raw->clusterSize;
	partBpb->resSectors = getI16(bpb_raw->resSectors);
	partBpb->fatCount = bpb_raw->fatCount;
	partBpb->rootSize = getI16(bpb_raw->rootSize);
	partBpb->fatSize = getI16(bpb_raw->fatSize);
	partBpb->trackSize = getI16(bpb_raw->trackSize);
	partBpb->headCount = getI16(bpb_raw->headCount);
	partBpb->hiddenCount = getI32(bpb_raw->hiddenCountL);
	partBpb->sectorCount = getI16(bpb_raw->sectorCountO);
	if (partBpb->sectorCount == 0) {
		partBpb->sectorCount = getI32(bpb_raw->sectorCount); // large partition
	}
	partBpb->partStart = part_rec->start;
	partBpb->rootDirStart = part_rec->start + (partBpb->fatCount * partBpb->fatSize) + partBpb->resSectors;
	for (ret = 0; ret < 8; ret++) {
		partBpb->fatId[ret] = bpb_raw->fatId[ret];
	}
	partBpb->fatId[ret] = 0;
	partBpb->rootDirCluster = 0;

	fat_determineFatType(partBpb);

	//fat32 specific info
	if (partBpb->fatType == FAT32 && partBpb->fatSize == 0) {
		partBpb->fatSize = getI32(bpb32_raw->fatSize32);
		partBpb->activeFat = getI16(bpb32_raw->fatStatus);
		if (partBpb->activeFat & 0x80) { //fat not synced
			partBpb->activeFat = (partBpb->activeFat & 0xF);
		} else {
			partBpb->activeFat = 0;
		}
		partBpb->rootDirStart = part_rec->start + (partBpb->fatCount * partBpb->fatSize) + partBpb->resSectors;
		partBpb->rootDirCluster = getI32(bpb32_raw->rootDirCluster);
		for (ret = 0; ret < 8; ret++) {
			partBpb->fatId[ret] = bpb32_raw->fatId[ret];
		}
		partBpb->fatId[ret] = 0;
	}
    return 1;
}

//---------------------------------------------------------------------------
/*
 returns:
 0 - no more dir entries
 1 - short name dir entry found
 2 - long name dir entry found
 3 - deleted dir entry found
*/
int fat_getDirentry(fat_direntry_sfn* dsfn, fat_direntry_lfn* dlfn, fat_direntry* dir ) {
	int i, j;
	int offset;
	int cont;

	//detect last entry - all zeros (slight modification by radad)
	if (dsfn->name[0] == 0) {
		return 0;
	}
	//detect deleted entry - it will be ignored
	if (dsfn->name[0] == 0xE5) {
		return 3;
	}

	//detect long filename
	if (dlfn->rshv == 0x0F && dlfn->reserved1 == 0x00 && dlfn->reserved2[0] == 0x00) {
		//long filename - almost whole direntry is unicode string - extract it
		offset = dlfn->entrySeq & 0x3f;
		offset--;
		offset = offset * 13;
		//name - 1st part
		cont = 1;
		for (i = 0; i < 10 && cont; i+=2) {
			if (dlfn->name1[i]==0 && dlfn->name1[i+1] == 0) {
				dir->name[offset] = 0; //terminate
				cont = 0; //stop
			} else {
				dir->name[offset] = dlfn->name1[i];
				offset++;
			}
		}
		//name - 2nd part
		for (i = 0; i < 12 && cont; i+=2) {
			if (dlfn->name2[i]==0 && dlfn->name2[i+1] == 0) {
				dir->name[offset] = 0; //terminate
				cont = 0; //stop
			} else {
				dir->name[offset] = dlfn->name2[i];
				offset++;
			}
		}
		//name - 3rd part
		for (i = 0; i < 4 && cont; i+=2) {
			if (dlfn->name3[i]==0 && dlfn->name3[i+1] == 0) {
				dir->name[offset] = 0; //terminate
				cont = 0; //stop
			} else {
				dir->name[offset] = dlfn->name3[i];
				offset++;
			}
		}
		if ((dlfn->entrySeq & 0x40)) { //terminate string flag
			dir->name[offset] = 0;
		}
		return 2;
	} else {
		//short filename
		//copy name

		for (i = 0; i < 8 && dsfn->name[i]!= 32; i++) {
			dir->sname[i] = dsfn->name[i];
			// NT�adaption for LaunchELF
			if (dsfn->reservedNT & 0x08 &&
			  dir->sname[i] >= 'A' && dir->sname[i] <= 'Z') {
				dir->sname[i] += 0x20;	//Force standard letters in name to lower case
			}
		}
		for (j=0; j < 3 && dsfn->ext[j] != 32; j++) {
			if (j == 0) {
				dir->sname[i] = '.';
				i++;
			}
			dir->sname[i+j] = dsfn->ext[j];
			// NT�adaption for LaunchELF
			if (dsfn->reservedNT & 0x10 &&
			  dir->sname[i+j] >= 'A' && dir->sname[i+j] <= 'Z') {
				dir->sname[i+j] += 0x20;	//Force standard letters in ext to lower case
			}
		}
		dir->sname[i+j] = 0; //terminate
		if (dir->name[0] == 0) { //long name desn't exit
			for (i =0 ; dir->sname[i] !=0; i++) dir->name[i] = dir->sname[i];
			dir->name[i] = 0;
		}
		dir->attr = dsfn->attr;
		dir->size = getI32(dsfn->size);
		dir->cluster = getI32_2(dsfn->clusterL, dsfn->clusterH);
		return 1;
	}

}

//---------------------------------------------------------------------------
//Set chain info (cluster/offset) cache
void fat_setFatDirChain(fat_driver* fatd, fat_dir* fatDir) {
	int i,j;
	int index;
	int chainSize;
	int nextChain;
	int clusterChainStart ;
	unsigned int fileCluster;
	int fileSize;
	int blockSize;


	XPRINTF("FAT driver: reading cluster chain  \n");
	fileCluster = fatDir->chain[0].cluster;

	if (fileCluster < 2) {
		XPRINTF("   early exit... \n");
		return;
	}

	fileSize = fatDir->size;
	blockSize = fileSize / DIR_CHAIN_SIZE;

	nextChain = 1;
	clusterChainStart = 0;
	j = 1;
	fileSize = 0;
	index = 0;

	while (nextChain) {
		chainSize = fat_getClusterChain(fatd, fileCluster, fatd->cbuf, MAX_DIR_CLUSTER, 1);
		if (chainSize == 0) { //the chain is full, but more chain parts exist
			chainSize = MAX_DIR_CLUSTER;
			fileCluster = fatd->cbuf[MAX_DIR_CLUSTER - 1];
		}else { //chain fits in the chain buffer completely - no next chain exist
			nextChain = 0;
		}
		//process the cluster chain (fatd->cbuf)
		for (i = clusterChainStart; i < chainSize; i++) {
			fileSize += (fatd->partBpb.clusterSize * fatd->partBpb.sectorSize);
			while (fileSize >= (j * blockSize) && j < DIR_CHAIN_SIZE) {
				fatDir->chain[j].cluster = fatd->cbuf[i];
				fatDir->chain[j].index = index;
				j++;
			}//ends "while"
			index++;
		}//ends "for"
		clusterChainStart = 1;
	}//ends "while"
	fatDir->lastCluster = fatd->cbuf[i-1];

#ifdef DEBUG_EXTREME //dlanor: I patched this because this bloat hid important stuff
	//debug
	printf("SEEK CLUSTER CHAIN CACHE fileSize=%i blockSize=%i \n", fatDir->size, blockSize);
	for (i = 0; i < DIR_CHAIN_SIZE; i++) {
		printf("index=%i cluster=%i offset= %i - %i start=%i \n",
			fatDir->chain[i].index, fatDir->chain[i].cluster,
			fatDir->chain[i].index * fatd->partBpb.clusterSize * fatd->partBpb.sectorSize,
			(fatDir->chain[i].index+1) * fatd->partBpb.clusterSize * fatd->partBpb.sectorSize,
			i * blockSize);
	}
#endif /* debug */
	XPRINTF("FAT driver: read cluster chain  done!\n");


}

//---------------------------------------------------------------------------
/* Set base attributes of direntry */
void fat_setFatDir(fat_driver* fatd, fat_dir* fatDir, fat_direntry_sfn* dsfn, fat_direntry* dir, int getClusterInfo ) {
	int i;
	unsigned char* srcName;

	XPRINTF("setting fat dir...\n");
	srcName = dir->sname;
	if (dir->name[0] != 0) { //long filename not empty
		srcName = dir->name;
	}
	//copy name
	for (i = 0; srcName[i] != 0; i++) fatDir->name[i] = srcName[i];
	fatDir->name[i] = 0; //terminate

	fatDir->attr = dsfn->attr;
	fatDir->size = dir->size;

	//created Date: Day, Month, Year-low, Year-high
	fatDir->cdate[0] = (dsfn->dateCreate[0] & 0x1F);
	fatDir->cdate[1] = (dsfn->dateCreate[0] >> 5) + ((dsfn->dateCreate[1] & 0x01) << 3 );
	i = 1980 + (dsfn->dateCreate[1] >> 1);
	fatDir->cdate[2] = (i & 0xFF);
	fatDir->cdate[3] = ((i & 0xFF00)>> 8);

	//created Time: Hours, Minutes, Seconds
	fatDir->ctime[0] = ((dsfn->timeCreate[1] & 0xF8) >> 3);
	fatDir->ctime[1] = ((dsfn->timeCreate[1] & 0x07) << 3) + ((dsfn->timeCreate[0] & 0xE0) >> 5);
	fatDir->ctime[6] = ((dsfn->timeCreate[0] & 0x1F) << 1);

	//accessed Date: Day, Month, Year-low, Year-high
	fatDir->adate[0] = (dsfn->dateAccess[0] & 0x1F);
	fatDir->adate[1] = (dsfn->dateAccess[0] >> 5) + ((dsfn->dateAccess[1] & 0x01) << 3 );
	i = 1980 + (dsfn->dateAccess[1] >> 1);
	fatDir->adate[2] = (i & 0xFF);
	fatDir->adate[3] = ((i & 0xFF00)>> 8);

	//modified Date: Day, Month, Year-low, Year-high
	fatDir->mdate[0] = (dsfn->dateWrite[0] & 0x1F);
	fatDir->mdate[1] = (dsfn->dateWrite[0] >> 5) + ((dsfn->dateWrite[1] & 0x01) << 3 );
	i = 1980 + (dsfn->dateWrite[1] >> 1);
	fatDir->mdate[2] = (i & 0xFF);
	fatDir->mdate[3] = ((i & 0xFF00)>> 8);

	//modified Time: Hours, Minutes, Seconds
	fatDir->mtime[0] = ((dsfn->timeWrite[1] & 0xF8) >> 3);
	fatDir->mtime[1] = ((dsfn->timeWrite[1] & 0x07) << 3) + ((dsfn->timeWrite[0] & 0xE0) >> 5);
	fatDir->mtime[2] = ((dsfn->timeWrite[0] & 0x1F) << 1);

	fatDir->chain[0].cluster = dir->cluster;
	fatDir->chain[0].index  = 0;
	if (getClusterInfo) {
		fat_setFatDirChain(fatd, fatDir);
	}
}

//---------------------------------------------------------------------------
int fat_getDirentrySectorData(fat_driver* fatd, unsigned int* startCluster, unsigned int* startSector, int* dirSector) {
	int chainSize;

	if (*startCluster == 0 && fatd->partBpb.fatType < FAT32) { //Root directory
		*startSector = fatd->partBpb.rootDirStart;
		*dirSector =  fatd->partBpb.rootSize / (fatd->partBpb.sectorSize / 32);
		return 0;
	}
	 //other directory or fat 32
	if (*startCluster == 0 && fatd->partBpb.fatType == FAT32) {
		*startCluster = fatd->partBpb.rootDirCluster;
	}
	*startSector = fat_cluster2sector(&fatd->partBpb, *startCluster);
	chainSize = fat_getClusterChain(fatd, *startCluster, fatd->cbuf, MAX_DIR_CLUSTER, 1);
	if (chainSize > 0) {
		*dirSector = chainSize * fatd->partBpb.clusterSize;
	} else {
		printf("FAT driver: Error getting cluster chain! startCluster=%i \n", *startCluster);
		return -EFAULT;
	}

	return chainSize;
}

//---------------------------------------------------------------------------
int fat_getDirentryStartCluster(fat_driver* fatd, unsigned char* dirName, unsigned int* startCluster, fat_dir* fatDir) {
	fat_direntry_sfn* dsfn;
	fat_direntry_lfn* dlfn;
	fat_direntry dir;
	int i;
	int dirSector;
	unsigned int startSector;
	int cont;
	int ret;
	int dirPos;
	mass_dev* mass_device = fatd->dev;

	cont = 1;
	XPRINTF("\n");
	XPRINTF("getting cluster for dir entry: %s \n", dirName);
	//clear name strings
	dir.sname[0] = 0;
	dir.name[0] = 0;

	ret = fat_getDirentrySectorData(fatd, startCluster, &startSector, &dirSector);
    if (ret < 0)
        return ret;

	XPRINTF("dirCluster=%i startSector=%i (%i) dirSector=%i \n", *startCluster, startSector, startSector * mass_device->sectorSize, dirSector);

	//go through first directory sector till the max number of directory sectors
	//or stop when no more direntries detected
	for (i = 0; i < dirSector && cont; i++) {
		unsigned char* sbuf = NULL; //sector buffer

		//At cluster borders, get correct sector from cluster chain buffer
		if ((*startCluster != 0) && (i % fatd->partBpb.clusterSize == 0)) {
			startSector = fat_cluster2sector(&fatd->partBpb, fatd->cbuf[(i / fatd->partBpb.clusterSize)]) -i;
		}

		ret = READ_SECTOR(fatd->dev, startSector + i, sbuf);
		if (ret < 0) {
			printf("FAT driver: read directory sector failed ! sector=%i\n", startSector + i);
			return -EIO;
		}
		XPRINTF("read sector ok, scanning sector for direntries...\n");
		dirPos = 0;

		// go through start of the sector till the end of sector
		while (cont &&  dirPos < fatd->partBpb.sectorSize) {
			dsfn = (fat_direntry_sfn*) (sbuf + dirPos);
			dlfn = (fat_direntry_lfn*) (sbuf + dirPos);
			cont = fat_getDirentry(dsfn, dlfn, &dir); //get single directory entry from sector buffer
			if (cont == 1) { //when short file name entry detected
				if (!(dir.attr & FAT_ATTR_VOLUME_LABEL)) { //not volume label
					if ((strEqual(dir.sname, dirName) == 0) ||
						(strEqual(dir.name, dirName) == 0) ) {
							XPRINTF("found! %s\n", dir.name);
							if (fatDir != NULL) { //fill the directory properties
								fat_setFatDir(fatd, fatDir, dsfn, &dir, 1);
							}
							*startCluster = dir.cluster;
							XPRINTF("direntry %s found at cluster: %i \n", dirName, dir.cluster);
							return dir.attr; //returns file or directory attr
						}
				}//ends "if(!(dir.attr & FAT_ATTR_VOLUME_LABEL))"
				//clear name strings
				dir.sname[0] = 0;
				dir.name[0] = 0;
			}//ends "if (cont == 1)"
			dirPos += 32; //directory entry of size 32 bytes
		}//ends "while"
	}//ends "for"
	XPRINTF("direntry %s not found! \n", dirName);
	return -EFAULT;
}

//---------------------------------------------------------------------------
// start cluster should be 0 - if we want to search from root directory
// otherwise the start cluster should be correct cluster of directory
// to search directory - set fatDir as NULL
int fat_getFileStartCluster(fat_driver* fatd, const char* fname, unsigned int* startCluster, fat_dir* fatDir) {
	unsigned char tmpName[257];
	int i;
	int offset;
	int cont;
	int ret;

	XPRINTF("Entering fat_getFileStartCluster\n");

	cont = 1;
	offset = 0;
	i=0;

	*startCluster = 0;
	if (fatDir != NULL) {
		memset(fatDir, 0, sizeof(fat_dir));
		fatDir->attr = FAT_ATTR_DIRECTORY;
	}
	if (fname[i] == '/') {
		i++;
	}

	for ( ; fname[i] !=0; i++) {
		if (fname[i] == '/') { //directory separator
			tmpName[offset] = 0; //terminate string
			ret = fat_getDirentryStartCluster(fatd, tmpName, startCluster, fatDir);
			if (ret < 0) {
				return -ENOENT;
			}
			offset = 0;
		} else{
			tmpName[offset] = fname[i];
			offset++;
		}
	}//ends "for"
	//and the final file
	tmpName[offset] = 0; //terminate string
	XPRINTF("Ready to get cluster for file \"%s\"\n", tmpName);
	if (fatDir != NULL) {
		//if the last char of the name was slash - the name was already found -exit
		if (offset == 0) {
			XPRINTF("Exiting from fat_getFileStartCluster with a folder\n");
			return 2;
		}
		ret = fat_getDirentryStartCluster(fatd, tmpName, startCluster, fatDir);
		if (ret < 0) {
			XPRINTF("Exiting from fat_getFileStartCluster with error %i\n", ret);
			return ret;
		}
		XPRINTF("file's startCluster found. Name=%s, cluster=%i \n", fname, *startCluster);
	}
	XPRINTF("Exiting from fat_getFileStartCluster with a file\n");
	return 1;
}

//---------------------------------------------------------------------------
void fat_getClusterAtFilePos(fat_driver* fatd, fat_dir* fatDir, unsigned int filePos, unsigned int* cluster, unsigned int* clusterPos) {
	int i;
	int blockSize;
	int j = (DIR_CHAIN_SIZE-1);

	blockSize = fatd->partBpb.clusterSize * fatd->partBpb.sectorSize;

	for (i = 0; i < (DIR_CHAIN_SIZE-1); i++) {
		if (fatDir->chain[i].index   * blockSize <= filePos &&
			fatDir->chain[i+1].index * blockSize >  filePos) {
				j = i;
				break;
			}
	}
	*cluster    = fatDir->chain[j].cluster;
	*clusterPos = (fatDir->chain[j].index * blockSize);
}

//---------------------------------------------------------------------------
int fat_readFile(fat_driver* fatd, fat_dir* fatDir, unsigned int filePos, unsigned char* buffer, int size) {
	int ret;
	int i,j;
	int chainSize;
	int nextChain;
	int startSector;
	int bufSize;
	int sectorSkip;
	int clusterSkip;
	int dataSkip;
	mass_dev* mass_device = fatd->dev;

	unsigned int bufferPos;
	unsigned int fileCluster;
	unsigned int clusterPos;

	int clusterChainStart;

	fat_getClusterAtFilePos(fatd, fatDir, filePos, &fileCluster, &clusterPos);
	sectorSkip = (filePos - clusterPos) / fatd->partBpb.sectorSize;
	clusterSkip = sectorSkip / fatd->partBpb.clusterSize;
	sectorSkip %= fatd->partBpb.clusterSize;
	dataSkip  = filePos  % fatd->partBpb.sectorSize;
	bufferPos = 0;

	XPRINTF("fileCluster = %i,  clusterPos= %i clusterSkip=%i, sectorSkip=%i dataSkip=%i \n",
		fileCluster, clusterPos, clusterSkip, sectorSkip, dataSkip);

	if (fileCluster < 2) {
		return 0;
	}

	bufSize = mass_device->sectorSize;
	nextChain = 1;
	clusterChainStart = 1;

	while (nextChain && size > 0 ) {
		chainSize = fat_getClusterChain(fatd, fileCluster, fatd->cbuf, MAX_DIR_CLUSTER, clusterChainStart);
		clusterChainStart = 0;
		if (chainSize == 0) { //the chain is full, but more chain parts exist
			chainSize = MAX_DIR_CLUSTER;
			fileCluster = fatd->cbuf[MAX_DIR_CLUSTER - 1];
		}else { //chain fits in the chain buffer completely - no next chain needed
			nextChain = 0;
		}
		while (clusterSkip >= MAX_DIR_CLUSTER) {
			chainSize = fat_getClusterChain(fatd, fileCluster, fatd->cbuf, MAX_DIR_CLUSTER, clusterChainStart);
			clusterChainStart = 0;
			if (chainSize == 0) { //the chain is full, but more chain parts exist
				chainSize = MAX_DIR_CLUSTER;
				fileCluster = fatd->cbuf[MAX_DIR_CLUSTER - 1];
			}else { //chain fits in the chain buffer completely - no next chain needed
				nextChain = 0;
			}
			clusterSkip -= MAX_DIR_CLUSTER;
		}

		//process the cluster chain (fatd->cbuf) and skip leading clusters if needed
		for (i = 0 + clusterSkip; i < chainSize && size > 0; i++) {
			//read cluster and save cluster content
			startSector = fat_cluster2sector(&fatd->partBpb, fatd->cbuf[i]);
			//process all sectors of the cluster (and skip leading sectors if needed)
			for (j = 0 + sectorSkip; j < fatd->partBpb.clusterSize && size > 0; j++) {
				unsigned char* sbuf = NULL; //sector buffer

				ret = READ_SECTOR(fatd->dev, startSector + j, sbuf);
				if (ret < 0) {
					printf("Read sector failed ! sector=%i\n", startSector + j);
					return bufferPos;
				}

				//compute exact size of transfered bytes
				if (size < bufSize) {
					bufSize = size + dataSkip;
				}
				if (bufSize > mass_device->sectorSize) {
					bufSize = mass_device->sectorSize;
				}
				XPRINTF("memcopy dst=%i, src=%i, size=%i  bufSize=%i \n", bufferPos, dataSkip, bufSize-dataSkip, bufSize);
				memcpy(buffer+bufferPos, sbuf + dataSkip, bufSize - dataSkip);
				size-= (bufSize - dataSkip);
				bufferPos +=  (bufSize - dataSkip);
				dataSkip = 0;
				bufSize = mass_device->sectorSize;
			}
			sectorSkip = 0;
		}
		clusterSkip = 0;
	}
	return bufferPos;
}

//---------------------------------------------------------------------------
void fat_mountCheck()
{
	mass_stor_configureDevices();
}

//---------------------------------------------------------------------------
int fat_connect(mass_dev* dev)
{
    int count = 0;
    int i;
	printf("usb fat: connect devId %i \n", dev->devId);

	fat_part partTable;
	if (fat_getPartitionTable(dev, &partTable) < 0)
        return -1;
        
    for ( i = 0; i < 4; i++)
    {
        if(
            partTable.record[ i ].sid == 6    ||
            partTable.record[ i ].sid == 4    ||
            partTable.record[ i ].sid == 1    ||  // fat 16, fat 12
            partTable.record[ i ].sid == 0x0B ||
            partTable.record[ i ].sid == 0x0C ||  // fat 32
            partTable.record[ i ].sid == 0x0E)    // fat 16 LBA
        {
            printf("usb_mass: mount partition %d\n", i);
            if (fat_mount(dev, &partTable.record[i]) >= 0)
                count++;
        }
    }

    if ( count == 0 )
    {  // no partition table detected
        // try to use "floppy" option
        printf("usb_mass: mount drive\n");
        part_record rec;
        rec.sid   =
        rec.start = 0;
        rec.count = dev->maxLBA;
        if (fat_mount(dev, &rec) < 0)
            return -1;
    }

	return 0;
}

//---------------------------------------------------------------------------
int fat_disconnect(mass_dev* dev)
{
	printf("usb fat: disconnect devId %i \n", dev->devId);

	int i;
	for (i = 0; i < NUM_DEVICES; ++i)
	{
		if (g_fatd[i] != NULL && g_fatd[i]->dev == dev)
			fat_forceUnmount(g_fatd[i]);
	}

	return 0;
}

//---------------------------------------------------------------------------
int fat_getNextDirentry(fat_driver* fatd, fat_dir_list* fatdlist, fat_dir* fatDir) {
	fat_direntry_sfn* dsfn;
	fat_direntry_lfn* dlfn;
	fat_direntry dir;
	int i;
	int dirSector;
	unsigned int startSector;
	int cont, new_entry;
	int ret;
	int dirPos;
	unsigned int dirCluster;
	mass_dev* mass_device = fatd->dev;

	//the getFirst function was not called
	if (fatdlist->direntryCluster == 0xFFFFFFFF || fatDir == NULL) {
		return -EFAULT;
	}

	dirCluster = fatdlist->direntryCluster;

	//clear name strings
	dir.sname[0] = 0;
	dir.name[0] = 0;

	ret = fat_getDirentrySectorData(fatd, &dirCluster, &startSector, &dirSector);
    if (ret < 0)
        return ret;

	XPRINTF("dirCluster=%i startSector=%i (%i) dirSector=%i \n", dirCluster, startSector, startSector * mass_device->sectorSize, dirSector);

	//go through first directory sector till the max number of directory sectors
	//or stop when no more direntries detected
  //dlanor: but avoid rescanning same areas redundantly (if possible)
	cont = 1;
	new_entry = 1;
	dirPos = (fatdlist->direntryIndex*32) % fatd->partBpb.sectorSize;
	for (i = ((fatdlist->direntryIndex*32) / fatd->partBpb.sectorSize); (i < dirSector) && cont; i++) {
		unsigned char* sbuf = NULL; //sector buffer

		//At cluster borders, get correct sector from cluster chain buffer
		if ((dirCluster != 0) && (new_entry || (i % fatd->partBpb.clusterSize == 0))) {
			startSector = fat_cluster2sector(&fatd->partBpb, fatd->cbuf[(i / fatd->partBpb.clusterSize)])
				-i + (i % fatd->partBpb.clusterSize);
			new_entry = 0;
		}
		ret = READ_SECTOR(fatd->dev, startSector + i, sbuf);
		if (ret < 0) {
			printf("Read directory  sector failed ! sector=%i\n", startSector + i);
			return -EIO;
		}

		// go through sector from current pos till its end
		while (cont &&  (dirPos < fatd->partBpb.sectorSize)) {
			dsfn = (fat_direntry_sfn*) (sbuf + dirPos);
			dlfn = (fat_direntry_lfn*) (sbuf + dirPos);
			cont = fat_getDirentry(dsfn, dlfn, &dir); //get a directory entry from sector
			fatdlist->direntryIndex++; //Note current entry processed
			if (cont == 1) { //when short file name entry detected
				fat_setFatDir(fatd, fatDir, dsfn, &dir, 0);
#if 0
                printf("fat_getNextDirentry %c%c%c%c%c%c %x %s %s\n",
                    (dir.attr & FAT_ATTR_VOLUME_LABEL) ? 'v' : '-',
                    (dir.attr & FAT_ATTR_DIRECTORY) ? 'd' : '-',
                    (dir.attr & FAT_ATTR_READONLY) ? 'r' : '-',
                    (dir.attr & FAT_ATTR_ARCHIVE) ? 'a' : '-',
                    (dir.attr & FAT_ATTR_SYSTEM) ? 's' : '-',
                    (dir.attr & FAT_ATTR_HIDDEN) ? 'h' : '-',
                    dir.attr,
                    dir.sname,
                    dir.name);
#endif
				return 1;
			}
			dirPos += 32; //directory entry of size 32 bytes
		}//ends "while"
		dirPos = 0;
	}//ends "for"
	// when we get this far - reset the direntry cluster
	fatdlist->direntryCluster = 0xFFFFFFFF; //no more files
	return 0; //indicate that no direntry is avalable
}

//---------------------------------------------------------------------------
int fat_getFirstDirentry(fat_driver* fatd, char * dirName, fat_dir_list* fatdlist, fat_dir* fatDir) {
	int ret;
	unsigned int startCluster = 0;

	ret = fat_getFileStartCluster(fatd, dirName, &startCluster, fatDir);
	if (ret < 0) { //dir name not found
		return -ENOENT;
	}
	//check that direntry is directory
	if (!(fatDir->attr & FAT_ATTR_DIRECTORY)) {
		return -ENOTDIR; //it's a file - exit
	}
	fatdlist->direntryCluster = startCluster;
	fatdlist->direntryIndex = 0;
	return fat_getNextDirentry(fatd, fatdlist, fatDir);
}

//---------------------------------------------------------------------------
int fat_mount(mass_dev* dev, part_record* rec)
{
	fat_driver* fatd = NULL;
	int i;
	for (i = 0; i < NUM_DEVICES && fatd == NULL; ++i)
	{
		if (g_fatd[i] == NULL)
		{
			XPRINTF("usb fat: allocate fat_driver %d!\n", sizeof(fat_driver));
			g_fatd[i] = malloc(sizeof(fat_driver));
			if (g_fatd[i] != NULL)
			{
				g_fatd[i]->dev = NULL;
			}
			fatd = g_fatd[i];
		}
		else if(g_fatd[i]->dev == NULL)
		{
			fatd = g_fatd[i];
		}
	}

	if (fatd == NULL)
	{
		printf("usb fat: unable to allocate drive!\n");
		return -1;
	}

	if (fatd->dev != NULL)
	{
		printf("usb fat: mount ERROR: alread mounted\n");
		fat_forceUnmount(fatd);
	}

	if (fat_getPartitionBootSector(dev, rec, &fatd->partBpb) < 0)
		return -1;

	fatd->dev = dev;
	fatd->deIdx = 0;
	fatd->clStackIndex = 0;
	fatd->clStackLast = 0;
	fatd->lastChainCluster = 0xFFFFFFFF;
	fatd->lastChainResult = -1;
	return 0;
}

//---------------------------------------------------------------------------
void fat_forceUnmount(fat_driver* fatd) //dlanor: added for disconnection events (flush impossible)
{
	XPRINTF("usb fat: forceUnmount devId %i \n", fatd->dev->devId);
	if(fatd->dev != NULL) {
		fatd->dev = NULL;
	}
}

//---------------------------------------------------------------------------
fat_driver * fat_getData(int device)
{
    if (device >= NUM_DEVICES)
        return NULL;

    fat_driver* fatd = g_fatd[device];
    if (fatd == NULL || fatd->dev == NULL)
        return NULL;
    else
        return fatd;
}

//---------------------------------------------------------------------------
int fat_readSector(fat_driver* fatd, unsigned int sector, unsigned char** buf)
{
	int ret;
	mass_dev* mass_device = fatd->dev;
	unsigned char* sbuf = NULL; //sector buffer

	ret = READ_SECTOR(fatd->dev, sector, sbuf);
	if (ret < 0) {
		printf("Read sector failed ! sector=%i\n", sector);
		return -EIO;
	}
	*buf = sbuf;
	return mass_device->sectorSize;
}
//---------------------------------------------------------------------------
//End of file:  fat_driver.c
//---------------------------------------------------------------------------
