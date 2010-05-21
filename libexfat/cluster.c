/*
	cluster.c (03.09.09)
	exFAT file system implementation library.

	Copyright (C) 2009, 2010  Andrew Nayenko

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "exfat.h"
#include <errno.h>
#include <string.h>

#define BMAP_GET(bitmap, index) ((bitmap)[(index) / 8] & (1u << ((index) % 8)))
#define BMAP_SET(bitmap, index) (bitmap)[(index) / 8] |= (1u << ((index) % 8))
#define BMAP_CLR(bitmap, index) (bitmap)[(index) / 8] &= ~(1u << ((index) % 8))

/*
 * Block to absolute offset.
 */
static off_t b2o(const struct exfat* ef, off_t block)
{
	return block << ef->sb->block_bits;
}

/*
 * Cluster to block.
 */
static off_t c2b(const struct exfat* ef, cluster_t cluster)
{
	if (cluster < EXFAT_FIRST_DATA_CLUSTER)
		exfat_bug("invalid cluster number %u", cluster);
	return le32_to_cpu(ef->sb->cluster_block_start) +
		((off_t) (cluster - EXFAT_FIRST_DATA_CLUSTER) << ef->sb->bpc_bits);
}

/*
 * Cluster to absolute offset.
 */
off_t exfat_c2o(const struct exfat* ef, cluster_t cluster)
{
	return b2o(ef, c2b(ef, cluster));
}

/*
 * Size in bytes to size in clusters (rounded upwards).
 */
static uint32_t bytes2clusters(const struct exfat* ef, uint64_t bytes)
{
	uint64_t cluster_size = CLUSTER_SIZE(*ef->sb);
	return (bytes + cluster_size - 1) / cluster_size;
}

cluster_t exfat_next_cluster(const struct exfat* ef,
		const struct exfat_node* node, cluster_t cluster)
{
	le32_t next;
	off_t fat_offset;

	if (cluster < EXFAT_FIRST_DATA_CLUSTER)
		exfat_bug("bad cluster 0x%x", cluster);

	if (IS_CONTIGUOUS(*node))
		return cluster + 1;
	fat_offset = b2o(ef, le32_to_cpu(ef->sb->fat_block_start))
		+ cluster * sizeof(cluster_t);
	exfat_read_raw(&next, sizeof(next), fat_offset, ef->fd);
	return le32_to_cpu(next);
}

cluster_t exfat_advance_cluster(const struct exfat* ef,
		struct exfat_node* node, uint32_t count)
{
	uint32_t i;

	if (node->fptr_index > count)
	{
		node->fptr_index = 0;
		node->fptr_cluster = node->start_cluster;
	}

	for (i = node->fptr_index; i < count; i++)
	{
		node->fptr_cluster = exfat_next_cluster(ef, node, node->fptr_cluster);
		if (CLUSTER_INVALID(node->fptr_cluster))
			break;
	}
	node->fptr_index = count;
	return node->fptr_cluster;
}

static cluster_t find_bit_and_set(uint8_t* bitmap, cluster_t start,
		cluster_t end)
{
	const cluster_t mid_start = (start + 7) / 8 * 8;
	const cluster_t mid_end = end / 8 * 8;
	cluster_t c;
	cluster_t byte;

	for (c = start; c < mid_start; c++)
		if (BMAP_GET(bitmap, c) == 0)
		{
			BMAP_SET(bitmap, c);
			return c + EXFAT_FIRST_DATA_CLUSTER;
		}

	for (byte = mid_start / 8; byte < mid_end / 8; byte++)
		if (bitmap[byte] != 0xff)
		{
			cluster_t bit;

			for (bit = 0; bit < 8; bit++)
				if (!(bitmap[byte] & (1u << bit)))
				{
					bitmap[byte] |= (1u << bit);
					return byte * 8 + bit + EXFAT_FIRST_DATA_CLUSTER;
				}
		}

	for (c = mid_end; c < end; c++)
		if (BMAP_GET(bitmap, c) == 0)
		{
			BMAP_SET(bitmap, c);
			return c + EXFAT_FIRST_DATA_CLUSTER;
		}

	return EXFAT_CLUSTER_END;
}

void exfat_flush_cmap(struct exfat* ef)
{
	exfat_write_raw(ef->cmap.chunk, (ef->cmap.chunk_size + 7) / 8,
			exfat_c2o(ef, ef->cmap.start_cluster), ef->fd);
	ef->cmap.dirty = 0;
}

static void set_next_cluster(const struct exfat* ef, int contiguous,
		cluster_t current, cluster_t next)
{
	off_t fat_offset;
	le32_t next_le32;

	if (contiguous)
		return;
	fat_offset = b2o(ef, le32_to_cpu(ef->sb->fat_block_start))
		+ current * sizeof(cluster_t);
	next_le32 = cpu_to_le32(next);
	exfat_write_raw(&next_le32, sizeof(next_le32), fat_offset, ef->fd);
}

static cluster_t allocate_cluster(struct exfat* ef, cluster_t hint)
{
	cluster_t cluster;

	hint -= EXFAT_FIRST_DATA_CLUSTER;
	if (hint >= ef->cmap.chunk_size)
		hint = 0;

	cluster = find_bit_and_set(ef->cmap.chunk, hint, ef->cmap.chunk_size);
	if (cluster == EXFAT_CLUSTER_END)
		cluster = find_bit_and_set(ef->cmap.chunk, 0, hint);
	if (cluster == EXFAT_CLUSTER_END)
	{
		exfat_error("no free space left");
		return EXFAT_CLUSTER_END;
	}

	ef->cmap.dirty = 1;
	/* FIXME update percentage of used space */
	return cluster;
}

static void free_cluster(struct exfat* ef, cluster_t cluster)
{
	if (CLUSTER_INVALID(cluster))
		exfat_bug("attempting to free invalid cluster");
	if (cluster < EXFAT_FIRST_DATA_CLUSTER ||
		cluster - EXFAT_FIRST_DATA_CLUSTER >= ef->cmap.size)
		exfat_bug("bad cluster 0x%x (0x%x)", cluster, ef->cmap.size);

	BMAP_CLR(ef->cmap.chunk, cluster - EXFAT_FIRST_DATA_CLUSTER);
	ef->cmap.dirty = 1;
	/* FIXME update percentage of used space */
}

static void make_noncontiguous(const struct exfat* ef, cluster_t first,
		cluster_t last)
{
	cluster_t c;

	for (c = first; c < last; c++)
		set_next_cluster(ef, 0, c, c + 1);
}

static int shrink_file(struct exfat* ef, struct exfat_node* node,
		uint32_t current, uint32_t difference);

static int grow_file(struct exfat* ef, struct exfat_node* node,
		uint32_t current, uint32_t difference)
{
	cluster_t previous;
	cluster_t next;
	uint32_t allocated = 0;

	if (difference == 0)
		exfat_bug("zero clusters count passed");

	if (node->start_cluster != EXFAT_CLUSTER_FREE)
	{
		/* get the last cluster of the file */
		previous = exfat_advance_cluster(ef, node, current - 1);
		if (CLUSTER_INVALID(previous))
		{
			exfat_error("invalid cluster in file");
			return -EIO;
		}
	}
	else
	{
		if (node->fptr_index != 0)
			exfat_bug("non-zero pointer index (%u)", node->fptr_index);
		/* file does not have clusters (i.e. is empty), allocate
		   the first one for it */
		previous = allocate_cluster(ef, 0);
		if (CLUSTER_INVALID(previous))
			return -ENOSPC;
		node->fptr_cluster = node->start_cluster = previous;
		allocated = 1;
		/* file consists of only one cluster, so it's contiguous */
		node->flags |= EXFAT_ATTRIB_CONTIGUOUS;
	}

	while (allocated < difference)
	{
		next = allocate_cluster(ef, previous + 1);
		if (CLUSTER_INVALID(next))
		{
			if (allocated != 0)
				shrink_file(ef, node, current + allocated, allocated);
			return -ENOSPC;
		}
		if (next != previous - 1 && IS_CONTIGUOUS(*node))
		{
			/* it's a pity, but we are not able to keep the file contiguous
			   anymore */
			make_noncontiguous(ef, node->start_cluster, previous);
			node->flags &= ~EXFAT_ATTRIB_CONTIGUOUS;
			node->flags |= EXFAT_ATTRIB_DIRTY;
		}
		set_next_cluster(ef, IS_CONTIGUOUS(*node), previous, next);
		previous = next;
		allocated++;
	}

	set_next_cluster(ef, IS_CONTIGUOUS(*node), previous, EXFAT_CLUSTER_END);
	return 0;
}

static int shrink_file(struct exfat* ef, struct exfat_node* node,
		uint32_t current, uint32_t difference)
{
	cluster_t previous;
	cluster_t next;

	if (difference == 0)
		exfat_bug("zero difference passed");
	if (node->start_cluster == EXFAT_CLUSTER_FREE)
		exfat_bug("unable to shrink empty file (%u clusters)", current);
	if (current < difference)
		exfat_bug("file underflow (%u < %u)", current, difference);

	/* crop the file */
	if (current > difference)
	{
		cluster_t last = exfat_advance_cluster(ef, node,
				current - difference - 1);
		if (CLUSTER_INVALID(last))
		{
			exfat_error("invalid cluster in file");
			return -EIO;
		}
		previous = exfat_next_cluster(ef, node, last);
		set_next_cluster(ef, IS_CONTIGUOUS(*node), last, EXFAT_CLUSTER_END);
	}
	else
	{
		previous = node->start_cluster;
		node->start_cluster = EXFAT_CLUSTER_FREE;
	}
	node->fptr_index = 0;
	node->fptr_cluster = node->start_cluster;

	/* free remaining clusters */
	while (difference--)
	{
		if (CLUSTER_INVALID(previous))
		{
			exfat_error("invalid cluster in file");
			return -EIO;
		}
		next = exfat_next_cluster(ef, node, previous);
		set_next_cluster(ef, IS_CONTIGUOUS(*node), previous,
				EXFAT_CLUSTER_FREE);
		free_cluster(ef, previous);
		previous = next;
	}
	return 0;
}

static void erase_raw(struct exfat* ef, size_t size, off_t offset)
{
	exfat_write_raw(ef->zero_block, size, offset, ef->fd);
}

static int erase_range(struct exfat* ef, struct exfat_node* node,
		uint64_t begin, uint64_t end)
{
	uint64_t block_boundary;
	cluster_t cluster;

	if (begin >= end)
		return 0;

	block_boundary = (node->size | (BLOCK_SIZE(*ef->sb) - 1)) + 1;
	cluster = exfat_advance_cluster(ef, node,
			node->size / CLUSTER_SIZE(*ef->sb));
	if (CLUSTER_INVALID(cluster))
	{
		exfat_error("invalid cluster in file");
		return -EIO;
	}
	/* erase from the beginning to the closest block boundary */
	erase_raw(ef, MIN(block_boundary, end) - node->size,
			exfat_c2o(ef, cluster) + node->size % CLUSTER_SIZE(*ef->sb));
	/* erase whole blocks */
	while (block_boundary < end)
	{
		if (block_boundary % CLUSTER_SIZE(*ef->sb) == 0)
			cluster = exfat_next_cluster(ef, node, cluster);
		erase_raw(ef, BLOCK_SIZE(*ef->sb),
			exfat_c2o(ef, cluster) + block_boundary % CLUSTER_SIZE(*ef->sb));
		block_boundary += BLOCK_SIZE(*ef->sb);
	}
	return 0;
}

int exfat_truncate(struct exfat* ef, struct exfat_node* node, uint64_t size)
{
	uint32_t c1 = bytes2clusters(ef, node->size);
	uint32_t c2 = bytes2clusters(ef, size);
	int rc = 0;

	if (node->size == size)
		return 0;

	if (c1 < c2)
		rc = grow_file(ef, node, c1, c2 - c1);
	else if (c1 > c2)
		rc = shrink_file(ef, node, c1, c1 - c2);

	if (rc != 0)
		return rc;

	rc = erase_range(ef, node, node->size, size);
	if (rc != 0)
		return rc;

	exfat_update_mtime(node);
	node->size = size;
	node->flags |= EXFAT_ATTRIB_DIRTY;
	return 0;
}

uint32_t exfat_count_free_clusters(struct exfat* ef)
{
	uint32_t free_clusters = 0;
	uint32_t i;

	for (i = 0; i < ef->cmap.size; i++)
		if (BMAP_GET(ef->cmap.chunk, i) == 0)
			free_clusters++;
	return free_clusters;
}
