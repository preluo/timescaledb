/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <catalog/pg_class.h>
#include <catalog/namespace.h>
#include <catalog/pg_trigger.h>
#include <catalog/indexing.h>
#include <catalog/pg_inherits.h>
#include <catalog/toasting.h>
#include <commands/trigger.h>
#include <commands/tablecmds.h>
#include <commands/defrem.h>
#include <tcop/tcopprot.h>
#include <access/htup.h>
#include <access/htup_details.h>
#include <access/xact.h>
#include <access/reloptions.h>
#include <nodes/makefuncs.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/hsearch.h>
#include <storage/lmgr.h>
#include <miscadmin.h>
#include <funcapi.h>
#include <fmgr.h>
#include <utils/datum.h>
#include <catalog/pg_type.h>
#include <utils/timestamp.h>
#include <nodes/execnodes.h>
#include <executor/executor.h>
#include <access/tupdesc.h>

#include "export.h"
#include "chunk.h"
#include "chunk_index.h"
#include "chunk_data_node.h"
#include "cross_module_fn.h"
#include "catalog.h"
#include "continuous_agg.h"
#include "cross_module_fn.h"
#include "dimension.h"
#include "dimension_slice.h"
#include "dimension_vector.h"
#include "errors.h"
#include "partitioning.h"
#include "hypertable.h"
#include "hypertable_data_node.h"
#include "hypercube.h"
#include "scanner.h"
#include "process_utility.h"
#include "trigger.h"
#include "compat.h"
#include "utils.h"
#include "interval.h"
#include "hypertable_cache.h"
#include "cache.h"
#include "bgw_policy/chunk_stats.h"
#include "scan_iterator.h"
#include "compression_chunk_size.h"
#include "extension.h"

/* Strictly speaking there is no danger to include it always, but it helps to remove it with PG11_LT
 * support. */
#if PG11_LT
#include "compat/fkeylist.h"
#endif

TS_FUNCTION_INFO_V1(ts_chunk_show_chunks);
TS_FUNCTION_INFO_V1(ts_chunk_drop_chunks);
TS_FUNCTION_INFO_V1(ts_chunks_in);
TS_FUNCTION_INFO_V1(ts_chunk_id_from_relid);
TS_FUNCTION_INFO_V1(ts_chunk_dml_blocker);
TS_FUNCTION_INFO_V1(ts_chunk_show);
TS_FUNCTION_INFO_V1(ts_chunk_create);

/* Used when processing scanned chunks */
typedef enum ChunkResult
{
	CHUNK_DONE,
	CHUNK_IGNORED,
	CHUNK_PROCESSED
} ChunkResult;

/*
 * Context for scanning and building a chunk from a stub.
 *
 * If found, the chunk will be created and the chunk pointer member is set in
 * the result. Optionally, a caller can pre-allocate the chunk member's memory,
 * which is useful if one, e.g., wants to fill in an memory-aligned array of
 * chunks.
 *
 * If the chunk is a tombstone (dropped flag set), then the Chunk will not be
 * created and instead is_dropped will be TRUE.
 */
typedef struct ChunkStubScanCtx
{
	ChunkStub *stub;
	Chunk *chunk;
	bool is_dropped;
} ChunkStubScanCtx;

static bool
chunk_stub_is_valid(const ChunkStub *stub, unsigned int expected_slices)
{
	return stub && stub->id > 0 && stub->constraints && expected_slices == stub->cube->num_slices &&
		   stub->cube->num_slices == stub->constraints->num_dimension_constraints;
}

typedef ChunkResult (*on_chunk_stub_func)(ChunkScanCtx *ctx, ChunkStub *stub);
static void chunk_scan_ctx_init(ChunkScanCtx *ctx, Hyperspace *hs, Point *p);
static void chunk_scan_ctx_destroy(ChunkScanCtx *ctx);
static void chunk_collision_scan(ChunkScanCtx *scanctx, Hypercube *cube);
static int chunk_scan_ctx_foreach_chunk_stub(ChunkScanCtx *ctx, on_chunk_stub_func on_chunk,
											 uint16 limit);
static Datum chunks_return_srf(FunctionCallInfo fcinfo);
static int chunk_cmp(const void *ch1, const void *ch2);
static Chunk *chunk_find(Hypertable *ht, Point *p, bool resurrect);
static void init_scan_by_qualified_table_name(ScanIterator *iterator, const char *schema_name,
											  const char *table_name);

#define IS_VALID_CHUNK(chunk)                                                                      \
	((chunk) && !(chunk)->fd.dropped && (chunk)->fd.id > 0 && (chunk)->fd.hypertable_id > 0 &&     \
	 OidIsValid((chunk)->table_id) && OidIsValid((chunk)->hypertable_relid) &&                     \
	 (chunk)->constraints && (chunk)->cube &&                                                      \
	 (chunk)->cube->num_slices == (chunk)->constraints->num_dimension_constraints &&               \
	 ((chunk)->relkind == RELKIND_RELATION ||                                                      \
	  ((chunk)->relkind == RELKIND_FOREIGN_TABLE && (chunk)->data_nodes != NIL)))

#define ASSERT_IS_VALID_CHUNK(chunk) Assert(IS_VALID_CHUNK(chunk))

#define ASSERT_IS_NULL_OR_VALID_CHUNK(chunk) Assert(chunk == NULL || IS_VALID_CHUNK(chunk))

static HeapTuple
chunk_formdata_make_tuple(const FormData_chunk *fd, TupleDesc desc)
{
	Datum values[Natts_chunk];
	bool nulls[Natts_chunk] = { false };

	memset(values, 0, sizeof(Datum) * Natts_chunk);

	values[AttrNumberGetAttrOffset(Anum_chunk_id)] = Int32GetDatum(fd->id);
	values[AttrNumberGetAttrOffset(Anum_chunk_hypertable_id)] = Int32GetDatum(fd->hypertable_id);
	values[AttrNumberGetAttrOffset(Anum_chunk_schema_name)] = NameGetDatum(&fd->schema_name);
	values[AttrNumberGetAttrOffset(Anum_chunk_table_name)] = NameGetDatum(&fd->table_name);
	/*when we insert a chunk the compressed chunk id is always NULL */
	if (fd->compressed_chunk_id == INVALID_CHUNK_ID)
		nulls[AttrNumberGetAttrOffset(Anum_chunk_compressed_chunk_id)] = true;
	else
	{
		values[AttrNumberGetAttrOffset(Anum_chunk_compressed_chunk_id)] =
			Int32GetDatum(fd->compressed_chunk_id);
	}
	values[AttrNumberGetAttrOffset(Anum_chunk_dropped)] = BoolGetDatum(fd->dropped);

	return heap_form_tuple(desc, values, nulls);
}

static void
chunk_formdata_fill(FormData_chunk *fd, const HeapTuple tuple, const TupleDesc desc)
{
	bool nulls[Natts_chunk];
	Datum values[Natts_chunk];

	memset(fd, 0, sizeof(FormData_chunk));
	heap_deform_tuple(tuple, desc, values, nulls);

	Assert(!nulls[AttrNumberGetAttrOffset(Anum_chunk_id)]);
	Assert(!nulls[AttrNumberGetAttrOffset(Anum_chunk_hypertable_id)]);
	Assert(!nulls[AttrNumberGetAttrOffset(Anum_chunk_schema_name)]);
	Assert(!nulls[AttrNumberGetAttrOffset(Anum_chunk_table_name)]);
	Assert(!nulls[AttrNumberGetAttrOffset(Anum_chunk_dropped)]);

	fd->id = DatumGetInt32(values[AttrNumberGetAttrOffset(Anum_chunk_id)]);
	fd->hypertable_id = DatumGetInt32(values[AttrNumberGetAttrOffset(Anum_chunk_hypertable_id)]);
	memcpy(&fd->schema_name,
		   DatumGetName(values[AttrNumberGetAttrOffset(Anum_chunk_schema_name)]),
		   NAMEDATALEN);
	memcpy(&fd->table_name,
		   DatumGetName(values[AttrNumberGetAttrOffset(Anum_chunk_table_name)]),
		   NAMEDATALEN);

	if (nulls[AttrNumberGetAttrOffset(Anum_chunk_compressed_chunk_id)])
		fd->compressed_chunk_id = INVALID_CHUNK_ID;
	else
		fd->compressed_chunk_id =
			DatumGetInt32(values[AttrNumberGetAttrOffset(Anum_chunk_compressed_chunk_id)]);

	fd->dropped = DatumGetBool(values[AttrNumberGetAttrOffset(Anum_chunk_dropped)]);
}

static void
chunk_insert_relation(Relation rel, Chunk *chunk)
{
	HeapTuple new_tuple;
	CatalogSecurityContext sec_ctx;

	new_tuple = chunk_formdata_make_tuple(&chunk->fd, RelationGetDescr(rel));

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_insert(rel, new_tuple);
	ts_catalog_restore_user(&sec_ctx);

	heap_freetuple(new_tuple);
}

void
ts_chunk_insert_lock(Chunk *chunk, LOCKMODE lock)
{
	Catalog *catalog = ts_catalog_get();
	Relation rel;

	rel = table_open(catalog_get_table_id(catalog, CHUNK), lock);
	chunk_insert_relation(rel, chunk);
	table_close(rel, lock);
}

typedef struct CollisionInfo
{
	Hypercube *cube;
	ChunkStub *colliding_chunk;
} CollisionInfo;

/*-
 * Align a chunk's hypercube in 'aligned' dimensions.
 *
 * Alignment ensures that chunks line up in a particular dimension, i.e., their
 * ranges should either be identical or not overlap at all.
 *
 * Non-aligned:
 *
 * ' [---------]      <- existing slice
 * '      [---------] <- calculated (new) slice
 *
 * To align the slices above there are two cases depending on where the
 * insertion point happens:
 *
 * Case 1 (reuse slice):
 *
 * ' [---------]
 * '      [--x------]
 *
 * The insertion point x falls within the range of the existing slice. We should
 * reuse the existing slice rather than creating a new one.
 *
 * Case 2 (cut to align):
 *
 * ' [---------]
 * '      [-------x-]
 *
 * The insertion point falls outside the range of the existing slice and we need
 * to cut the new slice to line up.
 *
 * ' [---------]
 * '        cut [---]
 * '
 *
 * Note that slice reuse (case 1) happens already when calculating the tentative
 * hypercube for the chunk, and is thus already performed once reaching this
 * function. Thus, we deal only with case 2 here. Also note that a new slice
 * might overlap in complicated ways, requiring multiple cuts. For instance,
 * consider the following situation:
 *
 * ' [------]   [-] [---]
 * '      [---x-------]  <- calculated slice
 *
 * This should but cut-to-align as follows:
 *
 * ' [------]   [-] [---]
 * '         [x]
 *
 * After a chunk collision scan, this function is called for each chunk in the
 * chunk scan context. Chunks in the scan context may have only a partial set of
 * slices if they only overlap in some, but not all, dimensions (see
 * illustrations below). Still, partial chunks may still be of interest for
 * alignment in a particular dimension. Thus, if a chunk has an overlapping
 * slice in an aligned dimension, we cut to not overlap with that slice.
 */
static ChunkResult
do_dimension_alignment(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	CollisionInfo *info = scanctx->data;
	Hypercube *cube = info->cube;
	Hyperspace *space = scanctx->space;
	ChunkResult res = CHUNK_IGNORED;
	int i;

	for (i = 0; i < space->num_dimensions; i++)
	{
		Dimension *dim = &space->dimensions[i];
		DimensionSlice *chunk_slice, *cube_slice;
		int64 coord = scanctx->point->coordinates[i];

		if (!dim->fd.aligned)
			continue;

		/*
		 * The stub might not have a slice for each dimension, so we cannot
		 * use array indexing. Fetch slice by dimension ID instead.
		 */
		chunk_slice = ts_hypercube_get_slice_by_dimension_id(stub->cube, dim->fd.id);

		if (NULL == chunk_slice)
			continue;

		cube_slice = cube->slices[i];

		/*
		 * Only cut-to-align if the slices collide and are not identical
		 * (i.e., if we are reusing an existing slice we should not cut it)
		 */
		if (!ts_dimension_slices_equal(cube_slice, chunk_slice) &&
			ts_dimension_slices_collide(cube_slice, chunk_slice))
		{
			ts_dimension_slice_cut(cube_slice, chunk_slice, coord);
			res = CHUNK_PROCESSED;
		}
	}

	return res;
}

/*
 * Calculate, and potentially set, a new chunk interval for an open dimension.
 */
static bool
calculate_and_set_new_chunk_interval(Hypertable *ht, Point *p)
{
	Hyperspace *hs = ht->space;
	Dimension *dim = NULL;
	Datum datum;
	int64 chunk_interval, coord;
	int i;

	if (!OidIsValid(ht->chunk_sizing_func) || ht->fd.chunk_target_size <= 0)
		return false;

	/* Find first open dimension */
	for (i = 0; i < hs->num_dimensions; i++)
	{
		dim = &hs->dimensions[i];

		if (IS_OPEN_DIMENSION(dim))
			break;

		dim = NULL;
	}

	/* Nothing to do if no open dimension */
	if (NULL == dim)
	{
		elog(WARNING,
			 "adaptive chunking enabled on hypertable \"%s\" without an open (time) dimension",
			 get_rel_name(ht->main_table_relid));

		return false;
	}

	coord = p->coordinates[i];
	datum = OidFunctionCall3(ht->chunk_sizing_func,
							 Int32GetDatum(dim->fd.id),
							 Int64GetDatum(coord),
							 Int64GetDatum(ht->fd.chunk_target_size));
	chunk_interval = DatumGetInt64(datum);

	/* Check if the function didn't set and interval or nothing changed */
	if (chunk_interval <= 0 || chunk_interval == dim->fd.interval_length)
		return false;

	/* Update the dimension */
	ts_dimension_set_chunk_interval(dim, chunk_interval);

	return true;
}

/*
 * Resolve chunk collisions.
 *
 * After a chunk collision scan, this function is called for each chunk in the
 * chunk scan context. We only care about chunks that have a full set of
 * slices/constraints that overlap with our tentative hypercube, i.e., they
 * fully collide. We resolve those collisions by cutting the hypercube.
 */
static ChunkResult
do_collision_resolution(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	CollisionInfo *info = scanctx->data;
	Hypercube *cube = info->cube;
	Hyperspace *space = scanctx->space;
	ChunkResult res = CHUNK_IGNORED;
	int i;

	if (stub->cube->num_slices != space->num_dimensions || !ts_hypercubes_collide(cube, stub->cube))
		return CHUNK_IGNORED;

	for (i = 0; i < space->num_dimensions; i++)
	{
		DimensionSlice *cube_slice = cube->slices[i];
		DimensionSlice *chunk_slice = stub->cube->slices[i];
		int64 coord = scanctx->point->coordinates[i];

		/*
		 * Only cut if we aren't reusing an existing slice and there is a
		 * collision
		 */
		if (!ts_dimension_slices_equal(cube_slice, chunk_slice) &&
			ts_dimension_slices_collide(cube_slice, chunk_slice))
		{
			ts_dimension_slice_cut(cube_slice, chunk_slice, coord);
			res = CHUNK_PROCESSED;

			/*
			 * Redo the collision check after each cut since cutting in one
			 * dimension might have resolved the collision in another
			 */
			if (!ts_hypercubes_collide(cube, stub->cube))
				return res;
		}
	}

	Assert(!ts_hypercubes_collide(cube, stub->cube));

	return res;
}

static ChunkResult
check_for_collisions(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	CollisionInfo *info = scanctx->data;
	Hypercube *cube = info->cube;
	Hyperspace *space = scanctx->space;

	/* Check if this chunk collides with our hypercube */
	if (stub->cube->num_slices == space->num_dimensions && ts_hypercubes_collide(cube, stub->cube))
	{
		info->colliding_chunk = stub;
		return CHUNK_DONE;
	}

	return CHUNK_IGNORED;
}

/*
 * Check if a (tentative) chunk collides with existing chunks.
 *
 * Return the colliding chunk. Note that the chunk is a stub and not a full
 * chunk.
 */
static ChunkStub *
chunk_collides(Hypertable *ht, Hypercube *hc)
{
	ChunkScanCtx scanctx;
	CollisionInfo info = {
		.cube = hc,
		.colliding_chunk = NULL,
	};

	chunk_scan_ctx_init(&scanctx, ht->space, NULL);

	/* Scan for all chunks that collide with the hypercube of the new chunk */
	chunk_collision_scan(&scanctx, hc);
	scanctx.data = &info;

	/* Find chunks that collide */
	chunk_scan_ctx_foreach_chunk_stub(&scanctx, check_for_collisions, 0);

	chunk_scan_ctx_destroy(&scanctx);

	return info.colliding_chunk;
}

/*-
 * Resolve collisions and perform alignmment.
 *
 * Chunks collide only if their hypercubes overlap in all dimensions. For
 * instance, the 2D chunks below collide because they overlap in both the X and
 * Y dimensions:
 *
 * ' _____
 * ' |    |
 * ' | ___|__
 * ' |_|__|  |
 * '   |     |
 * '   |_____|
 *
 * While the following chunks do not collide, although they still overlap in the
 * X dimension:
 *
 * ' _____
 * ' |    |
 * ' |    |
 * ' |____|
 * '   ______
 * '   |     |
 * '   |    *|
 * '   |_____|
 *
 * For the collision case above we obviously want to cut our hypercube to no
 * longer collide with existing chunks. However, the second case might still be
 * of interest for alignment in case X is an 'aligned' dimension. If '*' is the
 * insertion point, then we still want to cut the hypercube to ensure that the
 * dimension remains aligned, like so:
 *
 * ' _____
 * ' |    |
 * ' |    |
 * ' |____|
 * '       ___
 * '       | |
 * '       |*|
 * '       |_|
 *
 *
 * We perform alignment first as that might actually resolve chunk
 * collisions. After alignment we check for any remaining collisions.
 */
static void
chunk_collision_resolve(Hypertable *ht, Hypercube *cube, Point *p)
{
	ChunkScanCtx scanctx;
	CollisionInfo info = {
		.cube = cube,
		.colliding_chunk = NULL,
	};

	chunk_scan_ctx_init(&scanctx, ht->space, p);

	/* Scan for all chunks that collide with the hypercube of the new chunk */
	chunk_collision_scan(&scanctx, cube);
	scanctx.data = &info;

	/* Cut the hypercube in any aligned dimensions */
	chunk_scan_ctx_foreach_chunk_stub(&scanctx, do_dimension_alignment, 0);

	/*
	 * If there are any remaining collisions with chunks, then cut-to-fit to
	 * resolve those collisions
	 */
	chunk_scan_ctx_foreach_chunk_stub(&scanctx, do_collision_resolution, 0);

	chunk_scan_ctx_destroy(&scanctx);
}

int
ts_chunk_add_constraints(Chunk *chunk)
{
	int num_added;

	num_added = ts_chunk_constraints_add_dimension_constraints(chunk->constraints,
															   chunk->fd.id,
															   chunk->cube);
	num_added += ts_chunk_constraints_add_inheritable_constraints(chunk->constraints,
																  chunk->fd.id,
																  chunk->relkind,
																  chunk->hypertable_relid);

	return num_added;
}

/* applies the attributes and statistics target for columns on the hypertable
   to columns on the chunk */
static void
set_attoptions(Relation ht_rel, Oid chunk_oid)
{
	TupleDesc tupleDesc = RelationGetDescr(ht_rel);
	int natts = tupleDesc->natts;
	int attno;

	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char *attributeName = NameStr(attribute->attname);
		HeapTuple tuple;
		Datum options;
		bool isnull;

		/* Ignore dropped */
		if (attribute->attisdropped)
			continue;

		tuple = SearchSysCacheAttName(RelationGetRelid(ht_rel), attributeName);

		Assert(tuple != NULL);

		/*
		 * Pass down the attribute options (ALTER TABLE ALTER COLUMN SET
		 * attribute_option)
		 */
		options = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions, &isnull);

		if (!isnull)
		{
			AlterTableCmd *cmd = makeNode(AlterTableCmd);

			cmd->subtype = AT_SetOptions;
			cmd->name = attributeName;
			cmd->def = (Node *) untransformRelOptions(options);
			AlterTableInternal(chunk_oid, list_make1(cmd), false);
		}

		/*
		 * Pass down the attribute options (ALTER TABLE ALTER COLUMN SET
		 * STATISTICS)
		 */
		options = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attstattarget, &isnull);
		if (!isnull)
		{
			int32 target = DatumGetInt32(options);

			/* Don't do anything if it's set to the default */
			if (target != -1)
			{
				AlterTableCmd *cmd = makeNode(AlterTableCmd);

				cmd->subtype = AT_SetStatistics;
				cmd->name = attributeName;
				cmd->def = (Node *) makeInteger(target);
				AlterTableInternal(chunk_oid, list_make1(cmd), false);
			}
		}

		ReleaseSysCache(tuple);
	}
}

static void
create_toast_table(CreateStmt *stmt, Oid chunk_oid)
{
	/* similar to tcop/utility.c */
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	Datum toast_options =
		transformRelOptions((Datum) 0, stmt->options, "toast", validnsps, true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	NewRelationCreateToastTable(chunk_oid, toast_options);
}

#if PG12_GE
/*
 * Get the access method name for a relation.
 */
static char *
get_am_name_for_rel(Oid relid)
{
	HeapTuple tuple;
	Form_pg_class cform;
	Oid amoid;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	cform = (Form_pg_class) GETSTRUCT(tuple);
	amoid = cform->relam;
	ReleaseSysCache(tuple);

	return get_am_name(amoid);
}
#endif

/*
 * Create a chunk's table.
 *
 * A chunk inherits from the main hypertable and will have the same owner. Since
 * chunks can be created either in the TimescaleDB internal schema or in a
 * user-specified schema, some care has to be taken to use the right
 * permissions, depending on the case:
 *
 * 1. if the chunk is created in the internal schema, we create it as the
 * catalog/schema owner (i.e., anyone can create chunks there via inserting into
 * a hypertable, but can not do it via CREATE TABLE).
 *
 * 2. if the chunk is created in a user-specified "associated schema", then we
 * shouldn't use the catalog owner to create the table since that typically
 * implies super-user permissions. If we would allow that, anyone can specify
 * someone else's schema in create_hypertable() and create chunks in it without
 * having the proper permissions to do so. With this logic, the hypertable owner
 * must have permissions to create tables in the associated schema, or else
 * table creation will fail. If the schema doesn't yet exist, the table owner
 * instead needs the proper permissions on the database to create the schema.
 */
Oid
ts_chunk_create_table(Chunk *chunk, Hypertable *ht, const char *tablespacename)
{
	Relation rel;
	ObjectAddress objaddr;
	int sec_ctx;
	char *namespace = NameStr(ht->fd.schema_name);
	char *hyper_name = NameStr(ht->fd.table_name);

	/*
	 * The CreateForeignTableStmt embeds a regular CreateStmt, so we can use
	 * it to create both regular and foreign tables
	 */
	CreateForeignTableStmt stmt = {
		.base.type = T_CreateStmt,
		.base.relation =
			makeRangeVar(NameStr(chunk->fd.schema_name), NameStr(chunk->fd.table_name), 0),
		.base.inhRelations = list_make1(makeRangeVar(namespace, hyper_name, 0)),
		.base.tablespacename = tablespacename ? pstrdup(tablespacename) : NULL,
		/* Propagate storage options of the main table to a regular chunk
		 * table, but avoid using it for a foreign chunk table. */
		.base.options =
			(chunk->relkind == RELKIND_RELATION) ? ts_get_reloptions(ht->main_table_relid) : NIL,
#if PG12_GE
		.base.accessMethod = (chunk->relkind == RELKIND_RELATION) ?
								 get_am_name_for_rel(chunk->hypertable_relid) :
								 NULL,
#endif
	};
	Oid uid, saved_uid;

	Assert(chunk->hypertable_relid == ht->main_table_relid);

	rel = table_open(ht->main_table_relid, AccessShareLock);

	/*
	 * If the chunk is created in the internal schema, become the catalog
	 * owner, otherwise become the hypertable owner
	 */
	if (namestrcmp(&chunk->fd.schema_name, INTERNAL_SCHEMA_NAME) == 0)
		uid = ts_catalog_database_info_get()->owner_uid;
	else
		uid = rel->rd_rel->relowner;

	GetUserIdAndSecContext(&saved_uid, &sec_ctx);

	if (uid != saved_uid)
		SetUserIdAndSecContext(uid, sec_ctx | SECURITY_LOCAL_USERID_CHANGE);

	objaddr = DefineRelation(&stmt.base,
							 chunk->relkind,
							 rel->rd_rel->relowner,
							 NULL
#if !PG96
							 ,
							 NULL
#endif
	);

	if (chunk->relkind == RELKIND_RELATION)
	{
		/*
		 * need to create a toast table explicitly for some of the option
		 * setting to work
		 */
		create_toast_table(&stmt.base, objaddr.objectId);

		if (uid != saved_uid)
			SetUserIdAndSecContext(saved_uid, sec_ctx);
	}
	else if (chunk->relkind == RELKIND_FOREIGN_TABLE)
	{
		ChunkDataNode *cdn;

		if (list_length(chunk->data_nodes) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_TS_NO_DATA_NODES),
					 (errmsg("no data nodes associated with chunk \"%s\"",
							 get_rel_name(chunk->table_id)))));

		/*
		 * Use the first chunk data node as the "primary" to put in the foreign
		 * table
		 */
		cdn = linitial(chunk->data_nodes);
		stmt.base.type = T_CreateForeignServerStmt;
		stmt.servername = NameStr(cdn->fd.node_name);

		/* Create the foreign table catalog information */
		CreateForeignTable(&stmt, objaddr.objectId);

		/*
		 * Need to restore security context to execute remote commands as the
		 * original user
		 */
		if (uid != saved_uid)
			SetUserIdAndSecContext(saved_uid, sec_ctx);

		/* Create the corresponding chunk replicas on the remote data nodes */
		ts_cm_functions->create_chunk_on_data_nodes(chunk, ht);

		/* Record the remote data node chunk ID mappings */
		ts_chunk_data_node_insert_multi(chunk->data_nodes);
	}
	else
		elog(ERROR, "invalid relkind \"%c\" when creating chunk", chunk->relkind);

	set_attoptions(rel, objaddr.objectId);

	table_close(rel, AccessShareLock);

	return objaddr.objectId;
}

static List *
chunk_assign_data_nodes(Chunk *chunk, Hypertable *ht)
{
	List *htnodes;
	List *chunk_data_nodes = NIL;
	ListCell *lc;

	if (chunk->relkind != RELKIND_FOREIGN_TABLE)
		return NIL;

	if (ht->data_nodes == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_TS_NO_DATA_NODES),
				 (errmsg("no data nodes associated with hypertable \"%s\"",
						 get_rel_name(ht->main_table_relid)))));

	Assert(chunk->cube != NULL);

	htnodes = ts_hypertable_assign_chunk_data_nodes(ht, chunk->cube);
	Assert(htnodes != NIL);

	foreach (lc, htnodes)
	{
		HypertableDataNode *htnode = lfirst(lc);
		ForeignServer *foreign_server =
			GetForeignServerByName(NameStr(htnode->fd.node_name), false);
		ChunkDataNode *chunk_data_node = palloc0(sizeof(ChunkDataNode));

		/*
		 * Create a stub data node (partially filled in entry). This will be
		 * fully filled in and persisted to metadata tables once we create the
		 * remote tables during insert
		 */
		chunk_data_node->fd.chunk_id = chunk->fd.id;
		chunk_data_node->fd.node_chunk_id = -1;
		namestrcpy(&chunk_data_node->fd.node_name, foreign_server->servername);
		chunk_data_node->foreign_server_oid = foreign_server->serverid;
		chunk_data_nodes = lappend(chunk_data_nodes, chunk_data_node);
	}

	return chunk_data_nodes;
}

List *
ts_chunk_get_data_node_name_list(const Chunk *chunk)
{
	List *datanodes = NULL;
	ListCell *lc;

	foreach (lc, chunk->data_nodes)
	{
		ChunkDataNode *cdn = lfirst(lc);

		datanodes = lappend(datanodes, NameStr(cdn->fd.node_name));
	}

	return datanodes;
}

static inline const char *
get_chunk_name_suffix(const char relkind)
{
	if (relkind == RELKIND_FOREIGN_TABLE)
		return "dist_chunk";
	return "chunk";
}

/*
 * Create a chunk from the dimensional constraints in the given hypercube.
 *
 * The table name for the chunk can be given explicitly, or generated if
 * table_name is NULL. If the table name is generated, it will use the given
 * prefix or, if NULL, use the hypertable's associated table prefix. Similarly,
 * if schema_name is NULL it will use the hypertable's associated schema for
 * the chunk.
 */
static Chunk *
chunk_create_metadata_after_lock(Hypertable *ht, Hypercube *cube, const char *schema_name,
								 const char *table_name, const char *prefix)
{
	Hyperspace *hs = ht->space;
	Catalog *catalog = ts_catalog_get();
	CatalogSecurityContext sec_ctx;
	Chunk *chunk;
	const char relkind = hypertable_chunk_relkind(ht);

	if (NULL == schema_name || schema_name[0] == '\0')
		schema_name = NameStr(ht->fd.associated_schema_name);

	/* Create a new chunk based on the hypercube */
	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	chunk = ts_chunk_create_base(ts_catalog_table_next_seq_id(catalog, CHUNK),
								 hs->num_dimensions,
								 relkind);

	ts_catalog_restore_user(&sec_ctx);

	chunk->fd.hypertable_id = hs->hypertable_id;
	chunk->cube = cube;
	chunk->hypertable_relid = ht->main_table_relid;
	namestrcpy(&chunk->fd.schema_name, schema_name);

	if (NULL == table_name || table_name[0] == '\0')
	{
		int len;

		if (NULL == prefix)
			prefix = NameStr(ht->fd.associated_table_prefix);

		len = snprintf(chunk->fd.table_name.data,
					   NAMEDATALEN,
					   "%s_%d_%s",
					   prefix,
					   chunk->fd.id,
					   get_chunk_name_suffix(relkind));

		if (len >= NAMEDATALEN)
			elog(ERROR, "chunk table name too long");
	}
	else
		namestrcpy(&chunk->fd.table_name, table_name);

	/* Insert chunk */
	ts_chunk_insert_lock(chunk, RowExclusiveLock);

	/* Insert any new dimension slices */
	ts_dimension_slice_insert_multi(cube->slices, cube->num_slices, true);

	/* Add metadata for dimensional and inheritable constraints */
	ts_chunk_add_constraints(chunk);
	ts_chunk_constraints_insert_metadata(chunk->constraints);

	/* If this is a remote chunk we assign data nodes */
	if (chunk->relkind == RELKIND_FOREIGN_TABLE)
		chunk->data_nodes = chunk_assign_data_nodes(chunk, ht);

	return chunk;
}

static Oid
chunk_create_table_after_lock(Chunk *chunk, Hypertable *ht)
{
	/* Create the actual table relation for the chunk */
	const char *tablespace = ts_hypertable_select_tablespace_name(ht, chunk);

	chunk->table_id = ts_chunk_create_table(chunk, ht, tablespace);

	if (!OidIsValid(chunk->table_id))
		elog(ERROR, "could not create chunk table");

	/* Create the chunk's constraints, triggers, and indexes */
	ts_chunk_constraints_create(chunk->constraints,
								chunk->table_id,
								chunk->fd.id,
								chunk->hypertable_relid,
								chunk->fd.hypertable_id);

	if (chunk->relkind == RELKIND_RELATION)
	{
		ts_trigger_create_all_on_chunk(chunk);
		ts_chunk_index_create_all(chunk->fd.hypertable_id,
								  chunk->hypertable_relid,
								  chunk->fd.id,
								  chunk->table_id);
	}

	return chunk->table_id;
}

static void
init_scan_by_chunk_id(ScanIterator *iterator, int32 chunk_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(), CHUNK, CHUNK_ID_INDEX);
	ts_scan_iterator_scan_key_init(iterator,
								   Anum_chunk_idx_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(chunk_id));
}

static Chunk *
chunk_create_from_hypercube_after_lock(Hypertable *ht, Hypercube *cube, const char *schema_name,
									   const char *table_name, const char *prefix)
{
	Chunk *chunk;

	chunk = chunk_create_metadata_after_lock(ht, cube, schema_name, table_name, prefix);
	Assert(chunk != NULL);
	chunk_create_table_after_lock(chunk, ht);

	return chunk;
}

static Chunk *
chunk_create_from_point_after_lock(Hypertable *ht, Point *p, const char *schema_name,
								   const char *table_name, const char *prefix)
{
	Hyperspace *hs = ht->space;
	Hypercube *cube;

	/*
	 * If the user has enabled adaptive chunking, call the function to
	 * calculate and set the new chunk time interval.
	 */
	calculate_and_set_new_chunk_interval(ht, p);

	/* Calculate the hypercube for a new chunk that covers the tuple's point */
	cube = ts_hypercube_calculate_from_point(hs, p);

	/* Resolve collisions with other chunks by cutting the new hypercube */
	chunk_collision_resolve(ht, cube, p);

	return chunk_create_from_hypercube_after_lock(ht, cube, schema_name, table_name, prefix);
}

Chunk *
ts_chunk_find_or_create_without_cuts(Hypertable *ht, Hypercube *hc, const char *schema_name,
									 const char *table_name, bool *created)
{
	ChunkStub *stub;
	Chunk *chunk = NULL;

	LockRelationOid(ht->main_table_relid, ShareUpdateExclusiveLock);

	stub = chunk_collides(ht, hc);

	if (NULL == stub)
	{
		chunk = chunk_create_from_hypercube_after_lock(ht, hc, schema_name, table_name, NULL);

		if (NULL != created)
			*created = true;
	}
	else
	{
		/* We can only use an existing chunk if it has identical dimensional
		 * constraints. Otherwise, throw an error */
		if (!ts_hypercube_equal(stub->cube, hc))
			ereport(ERROR,
					(errcode(ERRCODE_TS_CHUNK_COLLISION),
					 errmsg("chunk creation failed due to collision")));

		/* chunk_collides only returned a stub, so we need to lookup the full
		 * chunk. */
		chunk = ts_chunk_get_by_id(stub->id, true);

		if (NULL != created)
			*created = false;
	}

	ASSERT_IS_VALID_CHUNK(chunk);

	return chunk;
}

/*
 * Create a chunk through insertion of a tuple at a given point.
 */
Chunk *
ts_chunk_create_from_point(Hypertable *ht, Point *p, const char *schema, const char *prefix)
{
	Chunk *chunk;

	/*
	 * Serialize chunk creation around a lock on the "main table" to avoid
	 * multiple processes trying to create the same chunk. We use a
	 * ShareUpdateExclusiveLock, which is the weakest lock possible that
	 * conflicts with itself. The lock needs to be held until transaction end.
	 */
	LockRelationOid(ht->main_table_relid, ShareUpdateExclusiveLock);

	/* Recheck if someone else created the chunk before we got the table lock */
	chunk = chunk_find(ht, p, true);

	if (NULL == chunk)
	{
		if (hypertable_is_distributed_member(ht))
			ereport(ERROR,
					(errcode(ERRCODE_TS_INTERNAL_ERROR),
					 errmsg("distributed hypertable member cannot create chunk on its own"),
					 errhint("chunk creation should only happen through an access node")));

		chunk = chunk_create_from_point_after_lock(ht, p, schema, NULL, prefix);
	}

	ASSERT_IS_VALID_CHUNK(chunk);

	return chunk;
}

ChunkStub *
ts_chunk_stub_create(int32 id, int16 num_constraints)
{
	ChunkStub *stub;

	stub = palloc0(sizeof(*stub));
	stub->id = id;

	if (num_constraints > 0)
		stub->constraints = ts_chunk_constraints_alloc(num_constraints, CurrentMemoryContext);

	return stub;
}

Chunk *
ts_chunk_create_base(int32 id, int16 num_constraints, const char relkind)
{
	Chunk *chunk;

	chunk = palloc0(sizeof(Chunk));
	chunk->fd.id = id;
	chunk->fd.compressed_chunk_id = INVALID_CHUNK_ID;
	chunk->relkind = relkind;

	if (num_constraints > 0)
		chunk->constraints = ts_chunk_constraints_alloc(num_constraints, CurrentMemoryContext);

	return chunk;
}

/*
 * Build a chunk from a chunk tuple and a stub.
 *
 * The stub allows the chunk to be constructed more efficiently. But if the stub
 * is not "valid", dimension slices and constraints are fully
 * rescanned/recreated.
 */
static Chunk *
chunk_build_from_tuple_and_stub(Chunk **chunkptr, TupleInfo *ti, const ChunkStub *stub)
{
	Chunk *chunk = NULL;
	int num_constraints_hint = stub ? stub->constraints->num_constraints : 2;

	if (NULL == chunkptr)
		chunkptr = &chunk;

	if (NULL == *chunkptr)
		*chunkptr = MemoryContextAllocZero(ti->mctx, sizeof(Chunk));

	chunk = *chunkptr;
	chunk_formdata_fill(&chunk->fd, ti->tuple, ti->desc);

	/*
	 * When searching for the chunk stub matching the dimensional point, we only
	 * scanned for dimensional constraints. We now need to rescan the
	 * constraints to also get the inherited constraints.
	 */
	chunk->constraints =
		ts_chunk_constraint_scan_by_chunk_id(chunk->fd.id, num_constraints_hint, ti->mctx);

	/* If a stub is provided then reuse its hypercube. Note that stubs that are
	 * results of a point or range scan might be incomplete (in terms of number
	 * of slices and constraints). Only a chunk stub that matches in all
	 * dimensions will have a complete hypercube. Thus, we need to check the
	 * validity of the stub before we can reuse it.
	 */
	if (chunk_stub_is_valid(stub, chunk->constraints->num_dimension_constraints))
	{
		MemoryContext oldctx = MemoryContextSwitchTo(ti->mctx);

		chunk->cube = ts_hypercube_copy(stub->cube);
		MemoryContextSwitchTo(oldctx);

		/*
		 * The hypercube slices were filled in during the scan. Now we need to
		 * sort them in dimension order.
		 */
		ts_hypercube_slice_sort(chunk->cube);
	}
	else
		chunk->cube = ts_hypercube_from_constraints(chunk->constraints, ti->mctx);

	return chunk;
}

static ScanFilterResult
chunk_tuple_dropped_filter(TupleInfo *ti, void *arg)
{
	ChunkStubScanCtx *stubctx = arg;
	bool isnull;
	Datum dropped = heap_getattr(ti->tuple, Anum_chunk_dropped, ti->desc, &isnull);

	Assert(!isnull);
	stubctx->is_dropped = DatumGetBool(dropped);

	return stubctx->is_dropped ? SCAN_EXCLUDE : SCAN_INCLUDE;
}

static ScanTupleResult
chunk_tuple_found(TupleInfo *ti, void *arg)
{
	ChunkStubScanCtx *stubctx = arg;
	Chunk *chunk;

	chunk = chunk_build_from_tuple_and_stub(&stubctx->chunk, ti, stubctx->stub);
	Assert(!chunk->fd.dropped);

	/* Fill in table relids. Note that we cannot do this in
	 * chunk_build_from_tuple_and_stub() since chunk_resurrect() also uses that
	 * function and, in that case, the chunk object is needed to create the data
	 * table and related objects. */
	chunk->table_id = get_relname_relid(chunk->fd.table_name.data,
										get_namespace_oid(chunk->fd.schema_name.data, true));
	chunk->hypertable_relid = ts_inheritance_parent_relid(chunk->table_id);
	chunk->relkind = get_rel_relkind(chunk->table_id);

	if (chunk->relkind == RELKIND_FOREIGN_TABLE)
		chunk->data_nodes = ts_chunk_data_node_scan_by_chunk_id(chunk->fd.id, ti->mctx);

	return SCAN_DONE;
}

/* Create a chunk by scanning on chunk ID. A stub must be provided as input. */
static Chunk *
chunk_create_from_stub(ChunkStubScanCtx *stubctx)
{
	ScanKeyData scankey[1];
	Catalog *catalog = ts_catalog_get();
	int num_found;
	ScannerCtx scanctx = {
		.table = catalog_get_table_id(catalog, CHUNK),
		.index = catalog_get_index(catalog, CHUNK, CHUNK_ID_INDEX),
		.nkeys = 1,
		.scankey = scankey,
		.data = stubctx,
		.filter = chunk_tuple_dropped_filter,
		.tuple_found = chunk_tuple_found,
		.lockmode = AccessShareLock,
		.scandirection = ForwardScanDirection,
	};

	/*
	 * Perform an index scan on chunk ID.
	 */
	ScanKeyInit(&scankey[0],
				Anum_chunk_idx_id,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(stubctx->stub->id));

	num_found = ts_scanner_scan(&scanctx);

	Assert(num_found == 0 || num_found == 1);

	if (stubctx->is_dropped)
	{
		Assert(num_found == 0);
		return NULL;
	}

	if (num_found != 1)
		elog(ERROR, "no chunk found with ID %d", stubctx->stub->id);

	Assert(NULL != stubctx->chunk);

	return stubctx->chunk;
}

/*
 * Initialize a chunk scan context.
 *
 * A chunk scan context is used to join chunk-related information from metadata
 * tables during scans.
 */
static void
chunk_scan_ctx_init(ChunkScanCtx *ctx, Hyperspace *hs, Point *p)
{
	struct HASHCTL hctl = {
		.keysize = sizeof(int32),
		.entrysize = sizeof(ChunkScanEntry),
		.hcxt = CurrentMemoryContext,
	};

	memset(ctx, 0, sizeof(*ctx));
	ctx->htab = hash_create("chunk-scan-context", 20, &hctl, HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);
	ctx->space = hs;
	ctx->point = p;
	ctx->lockmode = NoLock;
}

/*
 * Destroy the chunk scan context.
 *
 * This will free the hash table in the context, but not the chunks within since
 * they are not allocated on the hash tables memory context.
 */
static void
chunk_scan_ctx_destroy(ChunkScanCtx *ctx)
{
	hash_destroy(ctx->htab);
}

static inline void
dimension_slice_and_chunk_constraint_join(ChunkScanCtx *scanctx, DimensionVec *vec)
{
	int i;

	for (i = 0; i < vec->num_slices; i++)
	{
		/*
		 * For each dimension slice, find matching constraints. These will be
		 * saved in the scan context
		 */
		ts_chunk_constraint_scan_by_dimension_slice(vec->slices[i], scanctx, CurrentMemoryContext);
	}
}

/*
 * Scan for the chunk that encloses the given point.
 *
 * In each dimension there can be one or more slices that match the point's
 * coordinate in that dimension. Slices are collected in the scan context's hash
 * table according to the chunk IDs they are associated with. A slice might
 * represent the dimensional bound of multiple chunks, and thus is added to all
 * the hash table slots of those chunks. At the end of the scan there will be at
 * most one chunk that has a complete set of slices, since a point cannot belong
 * to two chunks.
 */
static void
chunk_point_scan(ChunkScanCtx *scanctx, Point *p)
{
	int i;

	/* Scan all dimensions for slices enclosing the point */
	for (i = 0; i < scanctx->space->num_dimensions; i++)
	{
		DimensionVec *vec;

		vec = dimension_slice_scan(scanctx->space->dimensions[i].fd.id, p->coordinates[i]);

		dimension_slice_and_chunk_constraint_join(scanctx, vec);
	}
}

/*
 * Scan for chunks that collide with the given hypercube.
 *
 * Collisions are determined using axis-aligned bounding box collision detection
 * generalized to N dimensions. Slices are collected in the scan context's hash
 * table according to the chunk IDs they are associated with. A slice might
 * represent the dimensional bound of multiple chunks, and thus is added to all
 * the hash table slots of those chunks. At the end of the scan, those chunks
 * that have a full set of slices are the ones that actually collide with the
 * given hypercube.
 *
 * Chunks in the scan context that do not collide (do not have a full set of
 * slices), might still be important for ensuring alignment in those dimensions
 * that require alignment.
 */
static void
chunk_collision_scan(ChunkScanCtx *scanctx, Hypercube *cube)
{
	int i;

	/* Scan all dimensions for colliding slices */
	for (i = 0; i < scanctx->space->num_dimensions; i++)
	{
		DimensionVec *vec;
		DimensionSlice *slice = cube->slices[i];

		vec = dimension_slice_collision_scan(slice->fd.dimension_id,
											 slice->fd.range_start,
											 slice->fd.range_end);

		/* Add the slices to all the chunks they are associated with */
		dimension_slice_and_chunk_constraint_join(scanctx, vec);
	}
}

/*
 * Apply a function to each stub in the scan context's hash table. If the limit
 * is greater than zero only a limited number of chunks will be processed.
 *
 * The chunk handler function (on_chunk_func) should return CHUNK_PROCESSED if
 * the chunk should be considered processed and count towards the given
 * limit. CHUNK_IGNORE can be returned to have a chunk NOT count towards the
 * limit. CHUNK_DONE counts the chunk but aborts processing irrespective of
 * whether the limit is reached or not.
 *
 * Returns the number of processed chunks.
 */
static int
chunk_scan_ctx_foreach_chunk_stub(ChunkScanCtx *ctx, on_chunk_stub_func on_chunk, uint16 limit)
{
	HASH_SEQ_STATUS status;
	ChunkScanEntry *entry;

	ctx->num_processed = 0;
	hash_seq_init(&status, ctx->htab);

	for (entry = hash_seq_search(&status); entry != NULL; entry = hash_seq_search(&status))
	{
		switch (on_chunk(ctx, entry->stub))
		{
			case CHUNK_DONE:
				ctx->num_processed++;
				hash_seq_term(&status);
				return ctx->num_processed;
			case CHUNK_PROCESSED:
				ctx->num_processed++;

				if (limit > 0 && ctx->num_processed == limit)
				{
					hash_seq_term(&status);
					return ctx->num_processed;
				}
				break;
			case CHUNK_IGNORED:
				break;
		}
	}

	return ctx->num_processed;
}

static ChunkResult
set_complete_chunk(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	if (chunk_stub_is_complete(stub, scanctx->space))
	{
		scanctx->data = stub;
#ifdef USE_ASSERT_CHECKING
		return CHUNK_PROCESSED;
#else
		return CHUNK_DONE;
#endif
	}
	return CHUNK_IGNORED;
}

typedef struct ChunkScanCtxAddChunkData
{
	Chunk *chunks;
	uint64 max_chunks;
	uint64 num_chunks;
} ChunkScanCtxAddChunkData;

static ChunkResult
chunk_scan_context_add_chunk(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	ChunkScanCtxAddChunkData *data = scanctx->data;
	ChunkStubScanCtx stubctx = {
		.chunk = &data->chunks[data->num_chunks],
		.stub = stub,
	};

	Assert(data->num_chunks < data->max_chunks);
	chunk_create_from_stub(&stubctx);

	if (stubctx.is_dropped)
		return CHUNK_IGNORED;

	data->num_chunks++;

	return CHUNK_PROCESSED;
}

/* Finds the first chunk that has a complete set of constraints. There should be
 * only one such chunk in the scan context when scanning for the chunk that
 * holds a particular tuple/point. */
static ChunkStub *
chunk_scan_ctx_get_chunk_stub(ChunkScanCtx *ctx)
{
	ctx->data = NULL;

#ifdef USE_ASSERT_CHECKING
	{
		int n = chunk_scan_ctx_foreach_chunk_stub(ctx, set_complete_chunk, 0);

		Assert(n == 0 || n == 1);
	}
#else
	chunk_scan_ctx_foreach_chunk_stub(ctx, set_complete_chunk, 1);
#endif

	return ctx->data;
}

/*
 * Resurrect a chunk from a tombstone.
 *
 * A chunk can be dropped while retaining its metadata as a tombstone. Such a
 * chunk is marked with dropped=true.
 *
 * This function resurrects such a dropped chunk based on the original metadata,
 * including recreating the table and related objects.
 */
static Chunk *
chunk_resurrect(Hypertable *ht, ChunkStub *stub)
{
	ScanIterator iterator;
	Chunk *chunk = NULL;
	int count = 0;

	Assert(NULL != stub->constraints);
	Assert(NULL != stub->cube);

	iterator = ts_scan_iterator_create(CHUNK, RowExclusiveLock, CurrentMemoryContext);
	init_scan_by_chunk_id(&iterator, stub->id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		HeapTuple new_tuple;

		Assert(count == 0 && chunk == NULL);
		chunk = chunk_build_from_tuple_and_stub(NULL, ti, stub);
		Assert(chunk->fd.dropped);

		/* Create data table and related objects */
		chunk->hypertable_relid = ht->main_table_relid;
		chunk->relkind = hypertable_chunk_relkind(ht);
		chunk->table_id = chunk_create_table_after_lock(chunk, ht);

		if (chunk->relkind == RELKIND_FOREIGN_TABLE)
			chunk->data_nodes = ts_chunk_data_node_scan_by_chunk_id(chunk->fd.id, ti->mctx);

		/* Finally, update the chunk tuple to no longer be a tombstone */
		chunk->fd.dropped = false;
		new_tuple = chunk_formdata_make_tuple(&chunk->fd, ti->desc);
		ts_catalog_update_tid(ti->scanrel, &ti->tuple->t_self, new_tuple);
		heap_freetuple(new_tuple);
		count++;

		/* Assume there's only one match. (We break early to avoid scanning
		 * also the updated tuple.) */
		break;
	}

	ts_scan_iterator_close(&iterator);

	Assert(count == 0 || count == 1);

	/* If count == 0 and chunk is NULL here, the tombstone (metadata) must
	 * have been removed before we had a chance to resurrect the chunk */
	return chunk;
}

/*
 * Find a chunk matching a point in a hypertable's N-dimensional hyperspace.
 *
 * This involves:
 *
 * 1) For each dimension:
 *	  - Find all dimension slices that match the dimension
 * 2) For each dimension slice:
 *	  - Find all chunk constraints matching the dimension slice
 * 3) For each matching chunk constraint
 *	  - Insert a chunk stub into a hash table and add the constraint to the chunk
 *	  - If chunk already exists in hash table, add the constraint to the chunk
 * 4) At the end of the scan, only one chunk in the hash table should have
 *	  N number of constraints. This is the matching chunk.
 *
 * NOTE: this function allocates transient data, e.g., dimension slice,
 * constraints and chunks, that in the end are not part of the returned
 * chunk. Therefore, this scan should be executed on a transient memory
 * context. The returned chunk needs to be copied into another memory context in
 * case it needs to live beyond the lifetime of the other data.
 */
static Chunk *
chunk_find(Hypertable *ht, Point *p, bool resurrect)
{
	ChunkStub *stub;
	Chunk *chunk = NULL;
	ChunkScanCtx ctx;

	/* The scan context will keep the state accumulated during the scan */
	chunk_scan_ctx_init(&ctx, ht->space, p);

	/* Abort the scan when the chunk is found */
	ctx.early_abort = true;

	/* Scan for the chunk matching the point */
	chunk_point_scan(&ctx, p);

	/* Find the stub that has N matching dimension constraints */
	stub = chunk_scan_ctx_get_chunk_stub(&ctx);

	chunk_scan_ctx_destroy(&ctx);

	if (NULL != stub)
	{
		ChunkStubScanCtx stubctx = {
			.stub = stub,
		};

		/* Fill in the rest of the chunk's data from the chunk table, unless
		 * the chunk is marked as dropped. */
		chunk = chunk_create_from_stub(&stubctx);

		/* Check if the found metadata is a tombstone (dropped=true) */
		if (stubctx.is_dropped)
		{
			Assert(chunk == NULL);

			/* Resurrect the chunk if requested by the caller */
			if (resurrect)
				chunk = chunk_resurrect(ht, stub);
		}
	}

	ASSERT_IS_NULL_OR_VALID_CHUNK(chunk);

	return chunk;
}

Chunk *
ts_chunk_find(Hypertable *ht, Point *p)
{
	return chunk_find(ht, p, false);
}

/*
 * Find all the chunks in hyperspace that include elements (dimension slices)
 * calculated by given range constraints and return the corresponding
 * ChunkScanCxt. It is the caller's responsibility to destroy this context after
 * usage.
 */
static ChunkScanCtx *
chunks_find_all_in_range_limit(Hypertable *ht, Dimension *time_dim, StrategyNumber start_strategy,
							   int64 start_value, StrategyNumber end_strategy, int64 end_value,
							   int limit, uint64 *num_found)
{
	ChunkScanCtx *ctx = palloc(sizeof(ChunkScanCtx));
	DimensionVec *slices;

	Assert(ht != NULL);

	/* must have been checked earlier that this is the case */
	Assert(time_dim != NULL);

	slices = ts_dimension_slice_scan_range_limit(time_dim->fd.id,
												 start_strategy,
												 start_value,
												 end_strategy,
												 end_value,
												 limit);

	/* The scan context will keep the state accumulated during the scan */
	chunk_scan_ctx_init(ctx, ht->space, NULL);

	/* No abort when the first chunk is found */
	ctx->early_abort = false;

	/* Scan for chunks that are in range */
	dimension_slice_and_chunk_constraint_join(ctx, slices);

	*num_found += hash_get_num_entries(ctx->htab);
	return ctx;
}

/*
 * Convert endpoint specifiers such as older than and newer than to an absolute
 * time. The rules for conversion are as follows:
 *
 * For PG INTERVAL input return now()-interval (this is the correct
 * interpretation for both older and newer than). For TIMESTAMP(TZ)/DATE/INTEGERS
 * return the internal time representation. Otherwise, ERROR.
 */
static int64
get_internal_time_from_endpoint_specifiers(Oid hypertable_relid, Dimension *time_dim,
										   Datum endpoint_datum, Oid endpoint_type,
										   const char *parameter_name, const char *caller_name)
{
	Oid partitioning_type = ts_dimension_get_partition_type(time_dim);
	FormData_ts_interval *ts_interval;

	ts_dimension_open_typecheck(endpoint_type, partitioning_type, caller_name);
	Assert(OidIsValid(endpoint_type));

	if (endpoint_type == INTERVALOID)
	{
		ts_interval = ts_interval_from_sql_input(hypertable_relid,
												 endpoint_datum,
												 endpoint_type,
												 parameter_name,
												 caller_name);
		return ts_time_value_to_internal(ts_interval_subtract_from_now(ts_interval, time_dim),
										 partitioning_type);
	}
	else
	{
		return ts_time_value_to_internal(endpoint_datum, endpoint_type);
	}
}

static ChunkScanCtx *
chunks_typecheck_and_find_all_in_range_limit(Hypertable *ht, Dimension *time_dim,
											 Datum older_than_datum, Oid older_than_type,
											 Datum newer_than_datum, Oid newer_than_type, int limit,
											 MemoryContext multi_call_memory_ctx, char *caller_name,
											 uint64 *num_found)
{
	ChunkScanCtx *chunk_ctx = NULL;
	int64 older_than = -1;
	int64 newer_than = -1;
	StrategyNumber start_strategy = InvalidStrategy;
	StrategyNumber end_strategy = InvalidStrategy;
	MemoryContext oldcontext;

	if (time_dim == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("no time dimension found")));

	if (older_than_type != InvalidOid)
	{
		older_than = get_internal_time_from_endpoint_specifiers(ht->main_table_relid,
																time_dim,
																older_than_datum,
																older_than_type,
																"older_than",
																caller_name);
		end_strategy = BTLessStrategyNumber;
	}

	if (newer_than_type != InvalidOid)
	{
		newer_than = get_internal_time_from_endpoint_specifiers(ht->main_table_relid,
																time_dim,
																newer_than_datum,
																newer_than_type,
																"newer_than",
																caller_name);
		start_strategy = BTGreaterEqualStrategyNumber;
	}

	if (older_than_type != InvalidOid && newer_than_type != InvalidOid && older_than < newer_than)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("When both older_than and newer_than are specified, "
						"older_than must refer to a time that is more recent than newer_than so "
						"that a valid overlapping range is specified")));

	oldcontext = MemoryContextSwitchTo(multi_call_memory_ctx);
	chunk_ctx = chunks_find_all_in_range_limit(ht,
											   time_dim,
											   start_strategy,
											   newer_than,
											   end_strategy,
											   older_than,
											   limit,
											   num_found);
	MemoryContextSwitchTo(oldcontext);

	return chunk_ctx;
}

static ChunkResult
append_chunk_common(ChunkScanCtx *scanctx, ChunkStub *stub, Chunk **chunk)
{
	ChunkStubScanCtx stubctx = {
		.stub = stub,
	};

	*chunk = NULL;

	if (!chunk_stub_is_complete(stub, scanctx->space))
		return CHUNK_IGNORED;

	/* Fill in the rest of the chunk's data from the chunk table */
	*chunk = chunk_create_from_stub(&stubctx);

	if (stubctx.is_dropped)
		return CHUNK_IGNORED;

	Assert(OidIsValid((*chunk)->table_id));

	if (scanctx->lockmode != NoLock)
		LockRelationOid((*chunk)->table_id, scanctx->lockmode);

	return CHUNK_PROCESSED;
}

static ChunkResult
append_chunk_oid(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	Chunk *chunk;
	ChunkResult res = append_chunk_common(scanctx, stub, &chunk);

	if (res == CHUNK_PROCESSED)
	{
		Assert(chunk != NULL);
		scanctx->data = lappend_oid(scanctx->data, chunk->table_id);
	}

	return res;
}

static ChunkResult
append_chunk(ChunkScanCtx *scanctx, ChunkStub *stub)
{
	Chunk *chunk;
	ChunkResult res = append_chunk_common(scanctx, stub, &chunk);

	if (res == CHUNK_PROCESSED)
	{
		Chunk **chunks = scanctx->data;

		Assert(chunk != NULL);

		if (NULL == chunks && scanctx->num_complete_chunks > 0)
			scanctx->data = chunks = palloc(sizeof(Chunk *) * scanctx->num_complete_chunks);

		Assert(scanctx->num_processed < scanctx->num_complete_chunks);
		chunks[scanctx->num_processed] = chunk;
	}

	return res;
}

static void *
chunk_find_all(Hypertable *ht, List *dimension_vecs, on_chunk_stub_func on_chunk, LOCKMODE lockmode,
			   unsigned int *num_chunks)
{
	ChunkScanCtx ctx;
	ListCell *lc;
	int num_processed;

	/* The scan context will keep the state accumulated during the scan */
	chunk_scan_ctx_init(&ctx, ht->space, NULL);

	/* Do not abort the scan when one chunk is found */
	ctx.early_abort = false;
	ctx.lockmode = lockmode;

	/* Scan all dimensions for slices enclosing the point */
	foreach (lc, dimension_vecs)
	{
		DimensionVec *vec = lfirst(lc);

		dimension_slice_and_chunk_constraint_join(&ctx, vec);
	}

	ctx.data = NULL;
	num_processed = chunk_scan_ctx_foreach_chunk_stub(&ctx, on_chunk, 0);

	if (NULL != num_chunks)
		*num_chunks = num_processed;

	chunk_scan_ctx_destroy(&ctx);

	return ctx.data;
}

Chunk **
ts_chunk_find_all(Hypertable *ht, List *dimension_vecs, LOCKMODE lockmode, unsigned int *num_chunks)
{
	Chunk **chunks = chunk_find_all(ht, dimension_vecs, append_chunk, lockmode, num_chunks);

#ifdef USE_ASSERT_CHECKING
	/* Assert that we never return dropped chunks */
	int i;

	for (i = 0; i < *num_chunks; i++)
		ASSERT_IS_VALID_CHUNK(chunks[i]);
#endif

	return chunks;
}

List *
ts_chunk_find_all_oids(Hypertable *ht, List *dimension_vecs, LOCKMODE lockmode)
{
	List *chunks = chunk_find_all(ht, dimension_vecs, append_chunk_oid, lockmode, NULL);

#ifdef USE_ASSERT_CHECKING
	/* Assert that we never return dropped chunks */
	ListCell *lc;

	foreach (lc, chunks)
	{
		Chunk *chunk = ts_chunk_get_by_relid(lfirst_oid(lc), true);
		ASSERT_IS_VALID_CHUNK(chunk);
	}
#endif

	return chunks;
}

/* show_chunks SQL function handler */
Datum
ts_chunk_show_chunks(PG_FUNCTION_ARGS)
{
	/*
	 * chunks_return_srf is called even when it is not the first call but only
	 * after doing some computation first
	 */
	if (SRF_IS_FIRSTCALL())
	{
		FuncCallContext *funcctx;

		Oid table_relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
		Datum older_than_datum = PG_GETARG_DATUM(1);
		Datum newer_than_datum = PG_GETARG_DATUM(2);

		/*
		 * get_fn_expr_argtype defaults to UNKNOWNOID if argument is NULL but
		 * making it InvalidOid makes the logic simpler later
		 */
		Oid older_than_type = PG_ARGISNULL(1) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 1);
		Oid newer_than_type = PG_ARGISNULL(2) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 2);

		funcctx = SRF_FIRSTCALL_INIT();

		funcctx->user_fctx = ts_chunk_get_chunks_in_time_range(table_relid,
															   older_than_datum,
															   newer_than_datum,
															   older_than_type,
															   newer_than_type,
															   "show_chunks",
															   funcctx->multi_call_memory_ctx,
															   &funcctx->max_calls);
	}

	return chunks_return_srf(fcinfo);
}

Chunk *
ts_chunk_get_chunks_in_time_range(Oid table_relid, Datum older_than_datum, Datum newer_than_datum,
								  Oid older_than_type, Oid newer_than_type, char *caller_name,
								  MemoryContext mctx, uint64 *num_chunks_returned)
{
	ListCell *lc;
	MemoryContext oldcontext;
	ChunkScanCtx **chunk_scan_ctxs;
	Chunk *chunks;
	ChunkScanCtxAddChunkData data;
	Cache *hypertable_cache;
	Hypertable *ht;
	Dimension *time_dim;
	Oid time_dim_type = InvalidOid;

	/*
	 * contains the list of hypertables which need to be considered. this is a
	 * list containing a single hypertable if we are passed a valid table
	 * OID. Otherwise, it will have the list of all hypertables in the system
	 */
	List *hypertables = NIL;
	int ht_index = 0;
	uint64 num_chunks = 0;
	int i;

	if (older_than_type != InvalidOid && newer_than_type != InvalidOid &&
		older_than_type != newer_than_type)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("older_than_type and newer_than_type should have the same type")));

	/*
	 * Cache outside the if block to make sure cached hypertable entry
	 * returned will still be valid in foreach block below
	 */
	hypertable_cache = ts_hypertable_cache_pin();
	if (!OidIsValid(table_relid))
	{
		hypertables = ts_hypertable_get_all();
	}
	else
	{
		ht = ts_hypertable_cache_get_entry(hypertable_cache, table_relid, CACHE_FLAG_NONE);
		hypertables = list_make1(ht);
	}

	oldcontext = MemoryContextSwitchTo(mctx);
	chunk_scan_ctxs = palloc(sizeof(ChunkScanCtx *) * list_length(hypertables));
	MemoryContextSwitchTo(oldcontext);
	foreach (lc, hypertables)
	{
		ht = lfirst(lc);

		if (ht->fd.compressed)
			elog(ERROR, "cannot call ts_chunk_get_chunks_in_time_range on a compressed hypertable");

		time_dim = hyperspace_get_open_dimension(ht->space, 0);

		if (time_dim_type == InvalidOid)
			time_dim_type = ts_dimension_get_partition_type(time_dim);

		/*
		 * Even though internally all time columns are represented as bigints,
		 * it is locally unclear what set of chunks should be returned if
		 * there are multiple tables on the system some of which care about
		 * timestamp when others do not. That is why, whenever there is any
		 * time dimension constraint given as an argument (older_than or
		 * newer_than) we make sure all hypertables have the time dimension
		 * type of the given type or through an error. This check is done
		 * across hypertables that is why it is not in the helper function
		 * below.
		 */
		if (time_dim_type != ts_dimension_get_partition_type(time_dim) &&
			(older_than_type != InvalidOid || newer_than_type != InvalidOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call \"%s\" on all hypertables "
							"when all hypertables do not have the same time dimension type",
							caller_name)));

		chunk_scan_ctxs[ht_index++] = chunks_typecheck_and_find_all_in_range_limit(ht,
																				   time_dim,
																				   older_than_datum,
																				   older_than_type,
																				   newer_than_datum,
																				   newer_than_type,
																				   -1,
																				   mctx,
																				   caller_name,
																				   &num_chunks);
	}

	chunks = MemoryContextAllocZero(mctx, sizeof(Chunk) * num_chunks);
	data = (ChunkScanCtxAddChunkData){
		.chunks = chunks,
		.max_chunks = num_chunks,
		.num_chunks = 0,
	};

	for (i = 0; i < list_length(hypertables); i++)
	{
		/* Get all the chunks from the context */
		chunk_scan_ctxs[i]->data = &data;
		chunk_scan_ctx_foreach_chunk_stub(chunk_scan_ctxs[i], chunk_scan_context_add_chunk, -1);
		/*
		 * only affects ctx.htab Got all the chunk already so can now safely
		 * destroy the context
		 */
		chunk_scan_ctx_destroy(chunk_scan_ctxs[i]);
	}

	*num_chunks_returned = data.num_chunks;
	qsort(chunks, *num_chunks_returned, sizeof(Chunk), chunk_cmp);

	ts_cache_release(hypertable_cache);

#ifdef USE_ASSERT_CHECKING
	do
	{
		uint64 i = 0;
		/* Assert that we never return dropped chunks */
		for (i = 0; i < *num_chunks_returned; i++)
			ASSERT_IS_VALID_CHUNK(&chunks[i]);
	} while (false);
#endif

	return chunks;
}

List *
ts_chunk_data_nodes_copy(Chunk *chunk)
{
	List *lcopy = NIL;
	ListCell *lc;

	foreach (lc, chunk->data_nodes)
	{
		ChunkDataNode *node = lfirst(lc);
		ChunkDataNode *copy = palloc(sizeof(ChunkDataNode));

		memcpy(copy, node, sizeof(ChunkDataNode));

		lcopy = lappend(lcopy, copy);
	}

	return lcopy;
}

Chunk *
ts_chunk_copy(Chunk *chunk)
{
	Chunk *copy;

	ASSERT_IS_VALID_CHUNK(chunk);
	copy = palloc(sizeof(Chunk));
	memcpy(copy, chunk, sizeof(Chunk));

	if (NULL != chunk->constraints)
		copy->constraints = ts_chunk_constraints_copy(chunk->constraints);

	if (NULL != chunk->cube)
		copy->cube = ts_hypercube_copy(chunk->cube);

	copy->data_nodes = ts_chunk_data_nodes_copy(chunk);

	return copy;
}

static int
chunk_scan_internal(int indexid, ScanKeyData scankey[], int nkeys, tuple_filter_func filter,
					tuple_found_func tuple_found, void *data, int limit, ScanDirection scandir,
					LOCKMODE lockmode, MemoryContext mctx)
{
	Catalog *catalog = ts_catalog_get();
	ScannerCtx ctx = {
		.table = catalog_get_table_id(catalog, CHUNK),
		.index = catalog_get_index(catalog, CHUNK, indexid),
		.nkeys = nkeys,
		.data = data,
		.scankey = scankey,
		.filter = filter,
		.tuple_found = tuple_found,
		.limit = limit,
		.lockmode = lockmode,
		.scandirection = scandir,
		.result_mctx = mctx,
	};

	return ts_scanner_scan(&ctx);
}

/*
 * Get a window of chunks that "precede" the given dimensional point.
 *
 * For instance, if the dimension is "time", then given a point in time the
 * function returns the recent chunks that come before the chunk that includes
 * that point. The count parameter determines the number or slices the window
 * should include in the given dimension. Note, that with multi-dimensional
 * partitioning, there might be multiple chunks in each dimensional slice that
 * all precede the given point. For instance, the example below shows two
 * different situations that each go "back" two slices (count = 2) in the
 * x-dimension, but returns two vs. eight chunks due to different
 * partitioning.
 *
 * '_____________
 * '|   |   | * |
 * '|___|___|___|
 * '
 * '
 * '____ ________
 * '|   |   | * |
 * '|___|___|___|
 * '|   |   |   |
 * '|___|___|___|
 * '|   |   |   |
 * '|___|___|___|
 * '|   |   |   |
 * '|___|___|___|
 *
 * Note that the returned chunks will be allocated on the given memory
 * context, including the list itself. So, beware of not leaking the list if
 * the chunks are later cached somewhere else.
 */
List *
ts_chunk_get_window(int32 dimension_id, int64 point, int count, MemoryContext mctx)
{
	List *chunks = NIL;
	DimensionVec *dimvec;
	int i;

	/* Scan for "count" slices that precede the point in the given dimension */
	dimvec = ts_dimension_slice_scan_by_dimension_before_point(dimension_id,
															   point,
															   count,
															   BackwardScanDirection,
															   mctx);

	/*
	 * For each slice, join with any constraints that reference the slice.
	 * There might be multiple constraints for each slice in case of
	 * multi-dimensional partitioning.
	 */
	for (i = 0; i < dimvec->num_slices; i++)
	{
		DimensionSlice *slice = dimvec->slices[i];
		ChunkConstraints *ccs = ts_chunk_constraints_alloc(1, mctx);
		int j;

		ts_chunk_constraint_scan_by_dimension_slice_id(slice->fd.id, ccs, mctx);

		/* For each constraint, find the corresponding chunk */
		for (j = 0; j < ccs->num_constraints; j++)
		{
			ChunkConstraint *cc = &ccs->constraints[j];
			Chunk *chunk = ts_chunk_get_by_id(cc->fd.chunk_id, true);
			MemoryContext old;

			chunk->constraints = ts_chunk_constraint_scan_by_chunk_id(chunk->fd.id, 1, mctx);
			chunk->cube = ts_hypercube_from_constraints(chunk->constraints, mctx);

			/* Allocate the list on the same memory context as the chunks */
			old = MemoryContextSwitchTo(mctx);
			chunks = lappend(chunks, chunk);
			MemoryContextSwitchTo(old);
		}
	}

#ifdef USE_ASSERT_CHECKING
	/* Assert that we never return dropped chunks */
	do
	{
		ListCell *lc;

		foreach (lc, chunks)
		{
			Chunk *chunk = lfirst(lc);
			ASSERT_IS_VALID_CHUNK(chunk);
		}
	} while (false);
#endif

	return chunks;
}

static Chunk *
chunk_scan_find(int indexid, ScanKeyData scankey[], int nkeys, MemoryContext mctx,
				bool fail_if_not_found)
{
	ChunkStubScanCtx stubctx = { 0 };
	Chunk *chunk;
	int num_found;

	num_found = chunk_scan_internal(indexid,
									scankey,
									nkeys,
									chunk_tuple_dropped_filter,
									chunk_tuple_found,
									&stubctx,
									1,
									ForwardScanDirection,
									AccessShareLock,
									mctx);

	Assert(num_found == 0 || (num_found == 1 && !stubctx.is_dropped));
	chunk = stubctx.chunk;

	switch (num_found)
	{
		case 0:
			if (fail_if_not_found)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("chunk not found")));
			break;
		case 1:
			ASSERT_IS_VALID_CHUNK(chunk);
			break;
		default:
			elog(ERROR, "expected a single chunk, found %d", num_found);
	}

	return chunk;
}

Chunk *
ts_chunk_get_by_name_with_memory_context(const char *schema_name, const char *table_name,
										 MemoryContext mctx, bool fail_if_not_found)
{
	NameData schema, table;
	ScanKeyData scankey[2];

	namestrcpy(&schema, schema_name);
	namestrcpy(&table, table_name);

	/*
	 * Perform an index scan on chunk name.
	 */
	ScanKeyInit(&scankey[0],
				Anum_chunk_schema_name_idx_schema_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				NameGetDatum(&schema));
	ScanKeyInit(&scankey[1],
				Anum_chunk_schema_name_idx_table_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				NameGetDatum(&table));

	return chunk_scan_find(CHUNK_SCHEMA_NAME_INDEX, scankey, 2, mctx, fail_if_not_found);
}

Chunk *
ts_chunk_get_by_relid(Oid relid, bool fail_if_not_found)
{
	char *schema;
	char *table;

	if (!OidIsValid(relid))
	{
		if (fail_if_not_found)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("invalid Oid")));
		else
			return NULL;
	}

	schema = get_namespace_name(get_rel_namespace(relid));
	table = get_rel_name(relid);
	return chunk_get_by_name(schema, table, fail_if_not_found);
}

Chunk *
ts_chunk_get_by_id(int32 id, bool fail_if_not_found)
{
	ScanKeyData scankey[1];

	/*
	 * Perform an index scan on chunk id.
	 */
	ScanKeyInit(&scankey[0], Anum_chunk_idx_id, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(id));

	return chunk_scan_find(CHUNK_ID_INDEX, scankey, 1, CurrentMemoryContext, fail_if_not_found);
}

/*
 * Number of chunks created after given chunk.
 * If chunk2.id > chunk1.id then chunk2 is created after chunk1
 */
int
ts_chunk_num_of_chunks_created_after(const Chunk *chunk)
{
	ScanKeyData scankey[1];

	/*
	 * Try to find chunks with a greater Id then a given chunk
	 */
	ScanKeyInit(&scankey[0],
				Anum_chunk_idx_id,
				BTGreaterStrategyNumber,
				F_INT4GT,
				Int32GetDatum(chunk->fd.id));

	return chunk_scan_internal(CHUNK_ID_INDEX,
							   scankey,
							   1,
							   NULL,
							   NULL,
							   NULL,
							   0,
							   ForwardScanDirection,
							   AccessShareLock,
							   CurrentMemoryContext);
}

/*
 * Simple scans provide lightweight ways to access chunk information without the
 * overhead of getting a full chunk (i.e., no extra metadata, like constraints,
 * are joined in). This function forms the basis of a number of lookup functions
 * that, e.g., translates a chunk relid to a chunk_id, or vice versa.
 */
static bool
chunk_simple_scan(ScanIterator *iterator, FormData_chunk *form, bool missing_ok)
{
	int count = 0;

	ts_scanner_foreach(iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(iterator);
		chunk_formdata_fill(form, ti->tuple, ti->desc);

		if (!form->dropped)
			count++;
	}

	Assert(count == 0 || count == 1);

	if (count == 0 && !missing_ok)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("chunk not found")));

	return count == 1;
}

static bool
chunk_simple_scan_by_name(const char *schema, const char *table, FormData_chunk *form,
						  bool missing_ok)
{
	ScanIterator iterator;

	if (schema == NULL || table == NULL)
		return false;

	iterator = ts_scan_iterator_create(CHUNK, AccessShareLock, CurrentMemoryContext);
	init_scan_by_qualified_table_name(&iterator, schema, table);

	return chunk_simple_scan(&iterator, form, missing_ok);
}

static bool
chunk_simple_scan_by_relid(Oid relid, FormData_chunk *form, bool missing_ok)
{
	bool found = false;

	if (OidIsValid(relid))
	{
		const char *table = get_rel_name(relid);

		if (table != NULL)
		{
			Oid nspid = get_rel_namespace(relid);
			const char *schema = get_namespace_name(nspid);

			found = chunk_simple_scan_by_name(schema, table, form, missing_ok);
		}
	}

	if (!found && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("chunk with relid %u not found", relid)));

	return found;
}

static bool
chunk_simple_scan_by_id(int32 chunk_id, FormData_chunk *form, bool missing_ok)
{
	ScanIterator iterator;

	iterator = ts_scan_iterator_create(CHUNK, AccessShareLock, CurrentMemoryContext);
	init_scan_by_chunk_id(&iterator, chunk_id);

	return chunk_simple_scan(&iterator, form, missing_ok);
}

/*
 * Lookup a Chunk ID from a chunk's relid.
 */
Datum
ts_chunk_id_from_relid(PG_FUNCTION_ARGS)
{
	static Oid last_relid = InvalidOid;
	static int32 last_id = 0;
	Oid relid = PG_GETARG_OID(0);
	FormData_chunk form;

	if (last_relid == relid)
		return last_id;

	chunk_simple_scan_by_relid(relid, &form, false);

	last_relid = relid;
	last_id = form.id;

	PG_RETURN_INT32(last_id);
}

bool
ts_chunk_exists_relid(Oid relid)
{
	FormData_chunk form;

	return chunk_simple_scan_by_relid(relid, &form, true);
}

/*
 * Get the relid of a chunk given its ID.
 */
Oid
ts_chunk_get_relid(int32 chunk_id, bool missing_ok)
{
	FormData_chunk form = { 0 };
	Oid relid = InvalidOid;

	if (chunk_simple_scan_by_id(chunk_id, &form, missing_ok))
	{
		Oid schemaid = get_namespace_oid(NameStr(form.schema_name), missing_ok);

		if (OidIsValid(schemaid))
			relid = get_relname_relid(NameStr(form.table_name), schemaid);
	}

	if (!OidIsValid(relid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("chunk with id %d not found", chunk_id)));

	return relid;
}

/*
 * Get the schema (namespace) of a chunk given its ID.
 *
 * This is a lightweight way to get the schema of a chunk without creating a
 * full Chunk object that joins in constraints, etc.
 */
Oid
ts_chunk_get_schema_id(int32 chunk_id, bool missing_ok)
{
	FormData_chunk form = { 0 };

	if (!chunk_simple_scan_by_id(chunk_id, &form, missing_ok))
		return InvalidOid;

	return get_namespace_oid(NameStr(form.schema_name), missing_ok);
}

bool
ts_chunk_get_id(const char *schema, const char *table, int32 *chunk_id, bool missing_ok)
{
	FormData_chunk form = { 0 };

	if (!chunk_simple_scan_by_name(schema, table, &form, missing_ok))
		return false;

	if (NULL != chunk_id)
		*chunk_id = form.id;

	return true;
}

/*
 * Results of deleting a chunk.
 *
 * A chunk can be deleted in two ways: (1) full delete of data and metadata,
 * (2) delete data but preserve metadata (marked with dropped=true). The
 * deletion mode (preserve or not) combined with the current state of the
 * "dropped" flag on a chunk metadata row leads to a cross-product resulting
 * in the following outcomes:
 */
typedef enum ChunkDeleteResult
{
	/* Deleted a live chunk */
	CHUNK_DELETED,
	/* Deleted a chunk previously marked "dropped" */
	CHUNK_DELETED_DROPPED,
	/* Marked a chunk as dropped instead of deleting */
	CHUNK_MARKED_DROPPED,
	/* Tried to mark a chunk as dropped when it was already marked */
	CHUNK_ALREADY_MARKED_DROPPED,
} ChunkDeleteResult;

/* Delete the chunk tuple.
 *
 * preserve_chunk_catalog_row - instead of deleting the row, mark it as dropped.
 * this is used when we need to preserve catalog information about the chunk
 * after dropping it. Currently only used when preserving continuous aggregates
 * on the chunk after the raw data was dropped. Otherwise, we'd have dangling
 * chunk ids left over in the materialization table. Preserve the space dimension
 * info about these chunks too.
 *
 * When chunk rows are preserved, the rows need to be updated to set the
 * 'dropped' flag to TRUE. But since this produces a new tuple into the
 * metadata table we will process also the new tuple in the same loop, which
 * is not only inefficent but could also lead to bugs. For now, we just ignore
 * those tuples (the CHUNK_ALREADY_MARKED_DROPPED case), but ideally we
 * shouldn't scan the updated tuples at all since it means double the number
 * of tuples to process.
 */
static ChunkDeleteResult
chunk_tuple_delete(TupleInfo *ti, DropBehavior behavior, bool preserve_chunk_catalog_row)
{
	FormData_chunk form;
	CatalogSecurityContext sec_ctx;
	ChunkConstraints *ccs = ts_chunk_constraints_alloc(2, ti->mctx);
	ChunkDeleteResult res;
	int i;

	chunk_formdata_fill(&form, ti->tuple, ti->desc);

	if (preserve_chunk_catalog_row && form.dropped)
		return CHUNK_ALREADY_MARKED_DROPPED;

	/* if only marking as deleted, keep the constraints and dimension info */
	if (!preserve_chunk_catalog_row)
	{
		ts_chunk_constraint_delete_by_chunk_id(form.id, ccs);

		/* Check for dimension slices that are orphaned by the chunk deletion */
		for (i = 0; i < ccs->num_constraints; i++)
		{
			ChunkConstraint *cc = &ccs->constraints[i];

			/*
			 * Delete the dimension slice if there are no remaining constraints
			 * referencing it
			 */
			if (is_dimension_constraint(cc) &&
				ts_chunk_constraint_scan_by_dimension_slice_id(cc->fd.dimension_slice_id,
															   NULL,
															   CurrentMemoryContext) == 0)
				ts_dimension_slice_delete_by_id(cc->fd.dimension_slice_id, false);
		}
	}

	ts_chunk_index_delete_by_chunk_id(form.id, true);
	ts_compression_chunk_size_delete(form.id);
	ts_chunk_data_node_delete_by_chunk_id(form.id);

	/* Delete any row in bgw_policy_chunk-stats corresponding to this chunk */
	ts_bgw_policy_chunk_stats_delete_by_chunk_id(form.id);

	if (form.compressed_chunk_id != INVALID_CHUNK_ID)
	{
		Chunk *compressed_chunk = ts_chunk_get_by_id(form.compressed_chunk_id, false);

		/* The chunk may have been delete by a CASCADE */
		if (compressed_chunk != NULL)
			/* Plain drop without preserving catalog row because this is the compressed chunk */
			ts_chunk_drop(compressed_chunk, behavior, DEBUG1);
	}

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);

	if (!preserve_chunk_catalog_row)
	{
		ts_catalog_delete(ti->scanrel, ti->tuple);

		if (form.dropped)
			res = CHUNK_DELETED_DROPPED;
		else
			res = CHUNK_DELETED;
	}
	else
	{
		HeapTuple new_tuple;

		Assert(!form.dropped);

		form.dropped = true;
		new_tuple = chunk_formdata_make_tuple(&form, ti->desc);
		ts_catalog_update_tid(ti->scanrel, &ti->tuple->t_self, new_tuple);
		heap_freetuple(new_tuple);
		res = CHUNK_MARKED_DROPPED;
	}

	ts_catalog_restore_user(&sec_ctx);

	return res;
}

static void
init_scan_by_qualified_table_name(ScanIterator *iterator, const char *schema_name,
								  const char *table_name)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(), CHUNK, CHUNK_SCHEMA_NAME_INDEX);
	ts_scan_iterator_scan_key_init(iterator,
								   Anum_chunk_schema_name_idx_schema_name,
								   BTEqualStrategyNumber,
								   F_NAMEEQ,
								   DirectFunctionCall1(namein, CStringGetDatum(schema_name)));
	ts_scan_iterator_scan_key_init(iterator,
								   Anum_chunk_schema_name_idx_table_name,
								   BTEqualStrategyNumber,
								   F_NAMEEQ,
								   DirectFunctionCall1(namein, CStringGetDatum(table_name)));
}

static int
chunk_delete(ScanIterator *iterator, DropBehavior behavior, bool preserve_chunk_catalog_row)
{
	int count = 0;

	ts_scanner_foreach(iterator)
	{
		ChunkDeleteResult res;

		res = chunk_tuple_delete(ts_scan_iterator_tuple_info(iterator),
								 behavior,
								 preserve_chunk_catalog_row);

		switch (res)
		{
			case CHUNK_DELETED:
			case CHUNK_MARKED_DROPPED:
				count++;
				break;
			case CHUNK_ALREADY_MARKED_DROPPED:
			case CHUNK_DELETED_DROPPED:
				break;
		}
	}

	return count;
}

static int
ts_chunk_delete_by_name_internal(const char *schema, const char *table, DropBehavior behavior,
								 bool preserve_chunk_catalog_row)
{
	ScanIterator iterator = ts_scan_iterator_create(CHUNK, RowExclusiveLock, CurrentMemoryContext);
	int count;

	init_scan_by_qualified_table_name(&iterator, schema, table);
	count = chunk_delete(&iterator, behavior, preserve_chunk_catalog_row);

	/* (schema,table) names and (hypertable_id) are unique so should only have
	 * dropped one chunk or none (if not found) */
	Assert(count == 1 || count == 0);

	return count;
}

int
ts_chunk_delete_by_name(const char *schema, const char *table, DropBehavior behavior)
{
	return ts_chunk_delete_by_name_internal(schema, table, behavior, false);
}

static int
ts_chunk_delete_by_relid(Oid relid, DropBehavior behavior, bool preserve_chunk_catalog_row)
{
	if (!OidIsValid(relid))
		return 0;

	return ts_chunk_delete_by_name_internal(get_namespace_name(get_rel_namespace(relid)),
											get_rel_name(relid),
											behavior,
											preserve_chunk_catalog_row);
}

static void
init_scan_by_hypertable_id(ScanIterator *iterator, int32 hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(), CHUNK, CHUNK_HYPERTABLE_ID_INDEX);
	ts_scan_iterator_scan_key_init(iterator,
								   Anum_chunk_hypertable_id_idx_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(hypertable_id));
}

int
ts_chunk_delete_by_hypertable_id(int32 hypertable_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CHUNK, RowExclusiveLock, CurrentMemoryContext);

	init_scan_by_hypertable_id(&iterator, hypertable_id);

	return chunk_delete(&iterator, DROP_RESTRICT, false);
}

bool
ts_chunk_exists_with_compression(int32 hypertable_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CHUNK, AccessShareLock, CurrentMemoryContext);
	bool found = false;

	init_scan_by_hypertable_id(&iterator, hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		bool isnull;
		heap_getattr(ts_scan_iterator_tuple_info(&iterator)->tuple,
					 Anum_chunk_compressed_chunk_id,
					 ts_scan_iterator_tuple_info(&iterator)->desc,
					 &isnull);
		if (!isnull)
		{
			found = true;
			break;
		}
	}
	ts_scan_iterator_close(&iterator);
	return found;
}

static void
init_scan_by_compressed_chunk_id(ScanIterator *iterator, int32 compressed_chunk_id)
{
	iterator->ctx.index =
		catalog_get_index(ts_catalog_get(), CHUNK, CHUNK_COMPRESSED_CHUNK_ID_INDEX);
	ts_scan_iterator_scan_key_init(iterator,
								   Anum_chunk_compressed_chunk_id_idx_compressed_chunk_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(compressed_chunk_id));
}

bool
ts_chunk_contains_compressed_data(Chunk *chunk)
{
	ScanIterator iterator = ts_scan_iterator_create(CHUNK, AccessShareLock, CurrentMemoryContext);
	bool found = false;

	init_scan_by_compressed_chunk_id(&iterator, chunk->fd.id);
	ts_scanner_foreach(&iterator)
	{
		Assert(!found);
		found = true;
	}
	return found;
}

List *
ts_chunk_get_chunk_ids_by_hypertable_id(int32 hypertable_id)
{
	List *chunkids = NIL;
	ScanIterator iterator = ts_scan_iterator_create(CHUNK, RowExclusiveLock, CurrentMemoryContext);

	init_scan_by_hypertable_id(&iterator, hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		bool isnull;
		Datum id = heap_getattr(ts_scan_iterator_tuple_info(&iterator)->tuple,
								Anum_chunk_id,
								ts_scan_iterator_tuple_info(&iterator)->desc,
								&isnull);
		if (!isnull)
			chunkids = lappend_int(chunkids, DatumGetInt32(id));
	}

	return chunkids;
}

static ChunkResult
chunk_recreate_constraint(ChunkScanCtx *ctx, ChunkStub *stub)
{
	ChunkConstraints *ccs = stub->constraints;
	ChunkStubScanCtx stubctx = {
		.stub = stub,
	};
	Chunk *chunk;
	int i;

	chunk = chunk_create_from_stub(&stubctx);

	if (stubctx.is_dropped)
		elog(ERROR, "should not be recreating constraints on dropped chunks");

	for (i = 0; i < ccs->num_constraints; i++)
		ts_chunk_constraint_recreate(&ccs->constraints[i], chunk->table_id);

	return CHUNK_PROCESSED;
}

void
ts_chunk_recreate_all_constraints_for_dimension(Hyperspace *hs, int32 dimension_id)
{
	DimensionVec *slices;
	ChunkScanCtx chunkctx;
	int i;

	slices = ts_dimension_slice_scan_by_dimension(dimension_id, 0);

	if (NULL == slices)
		return;

	chunk_scan_ctx_init(&chunkctx, hs, NULL);

	for (i = 0; i < slices->num_slices; i++)
		ts_chunk_constraint_scan_by_dimension_slice(slices->slices[i],
													&chunkctx,
													CurrentMemoryContext);

	chunk_scan_ctx_foreach_chunk_stub(&chunkctx, chunk_recreate_constraint, 0);
	chunk_scan_ctx_destroy(&chunkctx);
}

/*
 * Drops all FK constraints on a given chunk.
 * Currently it is used only for chunks, which have been compressed and
 * contain no data.
 */
void
ts_chunk_drop_fks(Chunk *const chunk)
{
	Relation rel;
	List *fks;
	ListCell *lc;

	ASSERT_IS_VALID_CHUNK(chunk);

	rel = table_open(chunk->table_id, AccessShareLock);
	fks = copy_fk_list_from_cache(RelationGetFKeyListCompat(rel));
	table_close(rel, AccessShareLock);

	foreach (lc, fks)
	{
		const ForeignKeyCacheInfoCompat *const fk = lfirst_node(ForeignKeyCacheInfoCompat, lc);
		ts_chunk_constraint_delete_by_constraint_name(chunk->fd.id,
													  get_constraint_name(fk->conoid),
													  true,
													  true);
	}
}

/*
 * Recreates all FK constraints on a chunk by using the constraints on the parent hypertable as a
 * template. Currently it is used only during chunk decompression, since FK constraints are dropped
 * during compression.
 */
void
ts_chunk_create_fks(Chunk *const chunk)
{
	Relation rel;
	List *fks;
	ListCell *lc;

	ASSERT_IS_VALID_CHUNK(chunk);

	rel = table_open(chunk->hypertable_relid, AccessShareLock);
	fks = copy_fk_list_from_cache(RelationGetFKeyListCompat(rel));
	table_close(rel, AccessShareLock);
	foreach (lc, fks)
	{
		ForeignKeyCacheInfoCompat *fk = lfirst_node(ForeignKeyCacheInfoCompat, lc);
		ts_chunk_constraint_create_on_chunk(chunk, fk->conoid);
	}
}

static ScanTupleResult
chunk_tuple_update_schema_and_table(TupleInfo *ti, void *data)
{
	FormData_chunk form;
	FormData_chunk *update = data;
	CatalogSecurityContext sec_ctx;
	HeapTuple new_tuple;

	chunk_formdata_fill(&form, ti->tuple, ti->desc);

	namecpy(&form.schema_name, &update->schema_name);
	namecpy(&form.table_name, &update->table_name);

	new_tuple = chunk_formdata_make_tuple(&form, ti->desc);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_update_tid(ti->scanrel, &ti->tuple->t_self, new_tuple);
	ts_catalog_restore_user(&sec_ctx);
	heap_freetuple(new_tuple);
	return SCAN_DONE;
}

static bool
chunk_update_form(FormData_chunk *form)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0], Anum_chunk_idx_id, BTEqualStrategyNumber, F_INT4EQ, form->id);

	return chunk_scan_internal(CHUNK_ID_INDEX,
							   scankey,
							   1,
							   NULL,
							   chunk_tuple_update_schema_and_table,
							   form,
							   0,
							   ForwardScanDirection,
							   AccessShareLock,
							   CurrentMemoryContext) > 0;
}

bool
ts_chunk_set_name(Chunk *chunk, const char *newname)
{
	namestrcpy(&chunk->fd.table_name, newname);

	return chunk_update_form(&chunk->fd);
}

bool
ts_chunk_set_schema(Chunk *chunk, const char *newschema)
{
	namestrcpy(&chunk->fd.schema_name, newschema);

	return chunk_update_form(&chunk->fd);
}

static ScanTupleResult
chunk_set_compressed_id_in_tuple(TupleInfo *ti, void *data)
{
	FormData_chunk form;
	HeapTuple new_tuple;
	CatalogSecurityContext sec_ctx;
	int32 compressed_chunk_id = *((int32 *) data);

	chunk_formdata_fill(&form, ti->tuple, ti->desc);
	form.compressed_chunk_id = compressed_chunk_id;
	new_tuple = chunk_formdata_make_tuple(&form, ti->desc);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_update_tid(ti->scanrel, &ti->tuple->t_self, new_tuple);
	ts_catalog_restore_user(&sec_ctx);
	heap_freetuple(new_tuple);

	return SCAN_DONE;
}

/*Assume permissions are already checked */
bool
ts_chunk_set_compressed_chunk(Chunk *chunk, int32 compressed_chunk_id, bool isnull)
{
	int32 compress_id;
	ScanKeyData scankey[1];
	ScanKeyInit(&scankey[0],
				Anum_chunk_idx_id,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(chunk->fd.id));
	if (isnull)
		compress_id = INVALID_CHUNK_ID;
	else
		compress_id = compressed_chunk_id;
	return chunk_scan_internal(CHUNK_ID_INDEX,
							   scankey,
							   1,
							   chunk_tuple_dropped_filter,
							   chunk_set_compressed_id_in_tuple,
							   &compress_id,
							   0,
							   ForwardScanDirection,
							   RowExclusiveLock,
							   CurrentMemoryContext) > 0;
}

/* Used as a tuple found function */
static ScanTupleResult
chunk_rename_schema_name(TupleInfo *ti, void *data)
{
	FormData_chunk form;
	HeapTuple new_tuple;
	CatalogSecurityContext sec_ctx;

	chunk_formdata_fill(&form, ti->tuple, ti->desc);
	/* Rename schema name */
	namestrcpy(&form.schema_name, (char *) data);
	new_tuple = chunk_formdata_make_tuple(&form, ti->desc);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_update_tid(ti->scanrel, &ti->tuple->t_self, new_tuple);
	ts_catalog_restore_user(&sec_ctx);
	heap_freetuple(new_tuple);
	return SCAN_CONTINUE;
}

/* Go through the internal chunk table and rename all matching schemas */
void
ts_chunks_rename_schema_name(char *old_schema, char *new_schema)
{
	NameData old_schema_name;
	ScanKeyData scankey[1];
	Catalog *catalog = ts_catalog_get();
	ScannerCtx scanctx = {
		.table = catalog_get_table_id(catalog, CHUNK),
		.index = catalog_get_index(catalog, CHUNK, CHUNK_SCHEMA_NAME_INDEX),
		.nkeys = 1,
		.scankey = scankey,
		.tuple_found = chunk_rename_schema_name,
		.data = new_schema,
		.lockmode = RowExclusiveLock,
		.scandirection = ForwardScanDirection,
	};

	namestrcpy(&old_schema_name, old_schema);

	ScanKeyInit(&scankey[0],
				Anum_chunk_schema_name_idx_schema_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				NameGetDatum(&old_schema_name));

	ts_scanner_scan(&scanctx);
}

static int
chunk_cmp(const void *ch1, const void *ch2)
{
	const Chunk *v1 = ((const Chunk *) ch1);
	const Chunk *v2 = ((const Chunk *) ch2);

	if (v1->fd.hypertable_id < v2->fd.hypertable_id)
		return -1;
	if (v1->fd.hypertable_id > v2->fd.hypertable_id)
		return 1;
	if (v1->table_id < v2->table_id)
		return -1;
	if (v1->table_id > v2->table_id)
		return 1;
	return 0;
}

/*
 * This is a helper set returning function (SRF) that takes a set returning function context and as
 * argument and returns oids extracted from funcctx->user_fctx (which is Chunk* array).
 * Note that the caller needs to be registered as a
 * set returning function for this to work.
 */
static Datum
chunks_return_srf(FunctionCallInfo fcinfo)
{
	FuncCallContext *funcctx;
	uint64 call_cntr;
	TupleDesc tupdesc;
	Chunk *result_set;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* Build a tuple descriptor for our result type */
		/* not quite necessary */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_SCALAR)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	result_set = (Chunk *) funcctx->user_fctx;

	/* do when there is more left to send */
	if (call_cntr < funcctx->max_calls)
		SRF_RETURN_NEXT(funcctx, result_set[call_cntr].table_id);
	else /* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

static void
ts_chunk_drop_internal(Chunk *chunk, DropBehavior behavior, int32 log_level,
					   bool preserve_catalog_row)
{
	ObjectAddress objaddr = {
		.classId = RelationRelationId,
		.objectId = chunk->table_id,
	};

	if (log_level >= 0)
		elog(log_level,
			 "dropping chunk %s.%s",
			 chunk->fd.schema_name.data,
			 chunk->fd.table_name.data);

	/* Remove the chunk from the chunk table */
	ts_chunk_delete_by_relid(chunk->table_id, behavior, preserve_catalog_row);

	/* Drop the table */
	performDeletion(&objaddr, behavior, 0);
}

void
ts_chunk_drop(Chunk *chunk, DropBehavior behavior, int32 log_level)
{
	ts_chunk_drop_internal(chunk, behavior, log_level, false);
}

void
ts_chunk_drop_preserve_catalog_row(Chunk *chunk, DropBehavior behavior, int32 log_level)
{
	ts_chunk_drop_internal(chunk, behavior, log_level, true);
}

static void
ts_chunk_drop_process_materialization(Oid hypertable_relid,
									  CascadeToMaterializationOption cascade_to_materializations,
									  Datum older_than_datum, Oid older_than_type,
									  Oid newer_than_type, Chunk *chunks, int num_chunks)
{
	Dimension *time_dimension;
	int64 older_than_time;
	int64 ignore_invalidation_older_than;
	int64 minimum_invalidation_time;
	int64 lowest_completion_time;
	List *continuous_aggs;
	ListCell *lc;
	Cache *hcache;
	Hypertable *ht;
	int i;

	FormData_continuous_agg cagg;

	/* nothing to do if also dropping materializations */
	if (cascade_to_materializations == CASCADE_TO_MATERIALIZATION_TRUE)
		return;

	Assert(cascade_to_materializations == CASCADE_TO_MATERIALIZATION_FALSE);

	if (OidIsValid(newer_than_type))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot use newer_than parameter to drop_chunks with "
						"cascade_to_materializations")));

	if (!OidIsValid(older_than_type))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("must use older_than parameter to drop_chunks with "
						"cascade_to_materializations")));

	ht = ts_hypertable_cache_get_cache_and_entry(hypertable_relid, CACHE_FLAG_NONE, &hcache);
	time_dimension = hyperspace_get_open_dimension(ht->space, 0);
	older_than_time = get_internal_time_from_endpoint_specifiers(ht->main_table_relid,
																 time_dimension,
																 older_than_datum,
																 older_than_type,
																 "older_than",
																 "drop_chunks");
	ignore_invalidation_older_than =
		ts_continuous_aggs_max_ignore_invalidation_older_than(ht->fd.id, &cagg);
	minimum_invalidation_time =
		ts_continuous_aggs_get_minimum_invalidation_time(ts_get_now_internal(time_dimension),
														 ignore_invalidation_older_than);

	/* minimum_invalidation_time is inclusive; older_than_time is exclusive */
	if (minimum_invalidation_time < older_than_time)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("older_than must be greater than the "
						"timescaledb.ignore_invalidation_older_than "
						"parameter of %s.%s",
						cagg.user_view_schema.data,
						cagg.user_view_name.data)));

	/* error for now, maybe better as a warning and ignoring the chunks? */
	/* We cannot move a completion threshold up transactionally without taking locks
	 * that would block the system. So, just bail. The completion threshold
	 * should be much higher than this anyway */
	lowest_completion_time = ts_continuous_aggs_min_completed_threshold(ht->fd.id, &cagg);
	if (lowest_completion_time < older_than_time)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the continuous aggregate %s.%s is too far behind",
						cagg.user_view_schema.data,
						cagg.user_view_name.data)));

	/* Lock all chunks in Exclusive mode, blocking everything but selects on the table. We have to
	 * block all modifications so that we cant get new invalidation entries. This makes sure that
	 * all future modifying txns on this data region will have a now() that higher than ours and
	 * thus will not invalidate. Otherwise, we could have an old txn with a now() in the past that
	 * all of a sudden decides to to insert data right after we process_invalidations. */
	for (i = 0; i < num_chunks; i++)
	{
		LockRelationOid(chunks[i].table_id, ExclusiveLock);
	}

	continuous_aggs = ts_continuous_aggs_find_by_raw_table_id(ht->fd.id);
	foreach (lc, continuous_aggs)
	{
		ContinuousAgg *ca = lfirst(lc);
		ContinuousAggMatOptions mat_options = {
			.verbose = true,
			.within_single_transaction = true,
			.process_only_invalidation = true,
			.invalidate_prior_to_time = older_than_time,
		};
		bool finished_all_invalidation = false;

		/* This will loop until all invalidations are done, each iteration of the loop will do
		 * max_interval_per_job's worth of data. We don't want to ignore max_interval_per_job here
		 * to avoid large sorts. */
		while (!finished_all_invalidation)
		{
			elog(NOTICE,
				 "making sure all invalidations for %s.%s have been processed prior to dropping "
				 "chunks",
				 NameStr(ca->data.user_view_schema),
				 NameStr(ca->data.user_view_name));
			finished_all_invalidation =
				ts_cm_functions->continuous_agg_materialize(ca->data.mat_hypertable_id,
															&mat_options);
		}
	}
	ts_cache_release(hcache);
}

/* Continuous agg materialization hypertables can be dropped
 * only if a user explicitly specifies the table name
 */
List *
ts_chunk_do_drop_chunks(Oid table_relid, Datum older_than_datum, Datum newer_than_datum,
						Oid older_than_type, Oid newer_than_type, bool cascade,
						CascadeToMaterializationOption cascades_to_materializations,
						int32 log_level, bool user_supplied_table_name, List **affected_data_nodes)

{
	uint64 i = 0;
	uint64 num_chunks = 0;
	Chunk *chunks;
	List *dropped_chunk_names = NIL;
	const char *schema_name, *table_name;
	int32 hypertable_id = ts_hypertable_relid_to_id(table_relid);
	bool has_continuous_aggs;
	List *data_nodes = NIL;

	Assert(OidIsValid(table_relid));

	ts_hypertable_permissions_check(table_relid, GetUserId());

	switch (ts_continuous_agg_hypertable_status(hypertable_id))
	{
		case HypertableIsMaterialization:
			if (user_supplied_table_name == false)
			{
				elog(ERROR, "cannot drop chunks on a continuous aggregate materialization table");
			}
			has_continuous_aggs = false;
			break;
		case HypertableIsMaterializationAndRaw:
			if (user_supplied_table_name == false)
			{
				elog(ERROR, "cannot drop chunks on a continuous aggregate materialization table");
			}
			has_continuous_aggs = true;
			break;
		case HypertableIsRawTable:
			if (cascades_to_materializations == CASCADE_TO_MATERIALIZATION_UNKNOWN)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cascade_to_materializations options must be set explicitly"),
						 errhint("Hypertables with continuous aggs must have the "
								 "cascade_to_materializations option set to either true or false "
								 "explicitly.")));
			has_continuous_aggs = true;
			break;
		default:
			has_continuous_aggs = false;
			cascades_to_materializations = CASCADE_TO_MATERIALIZATION_TRUE;
			break;
	}

	chunks = ts_chunk_get_chunks_in_time_range(table_relid,
											   older_than_datum,
											   newer_than_datum,
											   older_than_type,
											   newer_than_type,
											   "drop_chunks",
											   CurrentMemoryContext,
											   &num_chunks);

	if (has_continuous_aggs)
		ts_chunk_drop_process_materialization(table_relid,
											  cascades_to_materializations,
											  older_than_datum,
											  older_than_type,
											  newer_than_type,
											  chunks,
											  num_chunks);

	for (; i < num_chunks; i++)
	{
		char *chunk_name;
		ListCell *lc;

		ASSERT_IS_VALID_CHUNK(&chunks[i]);

		/* store chunk name for output */
		schema_name = quote_identifier(chunks[i].fd.schema_name.data);
		table_name = quote_identifier(chunks[i].fd.table_name.data);
		chunk_name = psprintf("%s.%s", schema_name, table_name);
		dropped_chunk_names = lappend(dropped_chunk_names, chunk_name);

		if (has_continuous_aggs && cascades_to_materializations == CASCADE_TO_MATERIALIZATION_FALSE)
			ts_chunk_drop_preserve_catalog_row(chunks + i,
											   (cascade ? DROP_CASCADE : DROP_RESTRICT),
											   log_level);
		else
			ts_chunk_drop(chunks + i, (cascade ? DROP_CASCADE : DROP_RESTRICT), log_level);

		/* Collect a list of affected data nodes so that we know which data
		 * nodes we need to drop chunks on */
		foreach (lc, chunks[i].data_nodes)
		{
			ChunkDataNode *cdn = lfirst(lc);

			data_nodes = list_append_unique_oid(data_nodes, cdn->foreign_server_oid);
		}
	}

	if (has_continuous_aggs && cascades_to_materializations == CASCADE_TO_MATERIALIZATION_TRUE)
	{
		ts_cm_functions->continuous_agg_drop_chunks_by_chunk_id(hypertable_id,
																&chunks,
																num_chunks,
																older_than_datum,
																newer_than_datum,
																older_than_type,
																newer_than_type,
																cascade,
																log_level,
																user_supplied_table_name);
	}

	if (affected_data_nodes)
		*affected_data_nodes = data_nodes;

	return dropped_chunk_names;
}

/*
 * This is a helper set returning function (SRF) that takes a set returning function context and as
 * argument and returns cstrings extracted from funcctx->user_fctx (which is a List).
 * Note that the caller needs to be registered as a
 * set returning function for this to work.
 */
static Datum
list_return_srf(FunctionCallInfo fcinfo)
{
	FuncCallContext *funcctx;
	uint64 call_cntr;
	TupleDesc tupdesc;
	List *result_set;
	Datum retval;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* Build a tuple descriptor for our result type */
		/* not quite necessary */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_SCALAR)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	result_set = (List *) funcctx->user_fctx;

	/* do when there is more left to send */
	if (call_cntr < funcctx->max_calls)
	{
		/* store return value and increment linked list */
		retval = CStringGetTextDatum(linitial(result_set));
		funcctx->user_fctx = list_delete_first(result_set);
		SRF_RETURN_NEXT(funcctx, retval);
	}
	else /* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

static void
drop_remote_chunks(FunctionCallInfo fcinfo, const Name schema_name, const Name table_name,
				   List *data_node_oids)
{
	/* Wildcard drops not supported on distributed hypertables */
	if (schema_name == NULL && table_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use wildcard to drop chunks on distributed hypertables"),
				 errhint("Drop chunks on each distributed hypertable individually.")));

	/* The schema name must be present when dropping remote chunks because the
	 * search path on the connection is always set to pg_catalog. Thus, the
	 * data node will not be able to resolve the same hypertables without the
	 * schema. */
	if (schema_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("schema required when dropping chunks on distributed hypertables")));

	/* Make sure the schema is set in case it was not given on the command line */
	FC_ARG(fcinfo, 2) = NameGetDatum(schema_name);
	FC_NULL(fcinfo, 2) = false;

	ts_cm_functions->func_call_on_data_nodes(fcinfo, data_node_oids);
}

Datum
ts_chunk_drop_chunks(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext;
	FuncCallContext *funcctx;
	ListCell *lc;
	List *ht_oids, *dc_names = NIL;
	Name table_name, schema_name;
	Datum older_than_datum, newer_than_datum;
	Oid older_than_type, newer_than_type;
	bool cascade, verbose;
	CascadeToMaterializationOption cascades_to_materializations;
	int elevel;
	bool user_supplied_table_name = true;
	List *data_node_oids = NIL;

	/*
	 * When past the first call of the SRF, dropping has already been completed,
	 * so we just return the next chunk in the list of dropped chunks.
	 */
	if (!SRF_IS_FIRSTCALL())
		return list_return_srf(fcinfo);

	table_name = PG_ARGISNULL(1) ? NULL : PG_GETARG_NAME(1);
	schema_name = PG_ARGISNULL(2) ? NULL : PG_GETARG_NAME(2);
	older_than_datum = PG_GETARG_DATUM(0);
	newer_than_datum = PG_GETARG_DATUM(4);

	/* Making types InvalidOid makes the logic simpler later */
	older_than_type = PG_ARGISNULL(0) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 0);
	newer_than_type = PG_ARGISNULL(4) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 4);
	cascade = PG_GETARG_BOOL(3);
	verbose = PG_ARGISNULL(5) ? false : PG_GETARG_BOOL(5);
	cascades_to_materializations =
		(PG_ARGISNULL(6) ? CASCADE_TO_MATERIALIZATION_UNKNOWN :
						   (PG_GETARG_BOOL(6) ? CASCADE_TO_MATERIALIZATION_TRUE :
												CASCADE_TO_MATERIALIZATION_FALSE));
	elevel = verbose ? INFO : DEBUG2;

	if (PG_ARGISNULL(0) && PG_ARGISNULL(4))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("older_than and newer_than timestamps provided to drop_chunks cannot both "
						"be NULL")));

	ht_oids = ts_hypertable_get_all_by_name(schema_name, table_name, CurrentMemoryContext);

	if (table_name != NULL)
	{
		if (ht_oids == NIL)
		{
			ContinuousAgg *ca = NULL;
			ca = ts_continuous_agg_find_userview_name(schema_name ? NameStr(*schema_name) : NULL,
													  NameStr(*table_name));
			if (ca == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_TS_HYPERTABLE_NOT_EXIST),
						 errmsg("\"%s\" is not a hypertable or a continuous aggregate view",
								NameStr(*table_name)),
						 errhint("It is only possible to drop chunks from a hypertable or "
								 "continuous aggregate view")));
			else
			{
				int32 matid = ca->data.mat_hypertable_id;
				Hypertable *mat_ht = ts_hypertable_get_by_id(matid);
				ht_oids = lappend_oid(ht_oids, mat_ht->main_table_relid);
			}
		}
	}
	else
		user_supplied_table_name = false;

	/* Initial multi function call setup */
	funcctx = SRF_FIRSTCALL_INIT();

	/* We need the table schema when dropping remote chunks because the search
	 * path on the connection is always set to pg_catalog. Thus we can only
	 * use the schema if it is already set or there is only one
	 * hypertable. The presence of a schema is enforced in
	 * ts_chunk_do_drop_chunks. */
	if (NULL == schema_name && list_length(ht_oids) == 1)
	{
		Oid nspid = get_rel_namespace(linitial_oid(ht_oids));
		schema_name =
			DatumGetName(DirectFunctionCall1(namein, CStringGetDatum(get_namespace_name(nspid))));
	}

	/* Drop chunks and build list of dropped chunks */
	foreach (lc, ht_oids)
	{
		Oid table_relid = lfirst_oid(lc);
		List *fk_relids = NIL;
		List *dc_temp = NIL;
		ListCell *lf;

		ts_hypertable_permissions_check(table_relid, GetUserId());

		/* get foreign key tables associated with the hypertable */
		{
			List *cachedfkeys = NIL;
			ListCell *lf;
			Relation table_rel;

			table_rel = table_open(table_relid, AccessShareLock);

			/*
			 * this list is from the relcache and can disappear with a cache
			 * flush, so no further catalog access till we save the fk relids
			 */
			cachedfkeys = RelationGetFKeyList(table_rel);
			foreach (lf, cachedfkeys)
			{
				ForeignKeyCacheInfo *cachedfk = (ForeignKeyCacheInfo *) lfirst(lf);

				/*
				 * conrelid should always be that of the table we're
				 * considering
				 */
				Assert(cachedfk->conrelid == RelationGetRelid(table_rel));
				fk_relids = lappend_oid(fk_relids, cachedfk->confrelid);
			}
			table_close(table_rel, AccessShareLock);
		}

		/*
		 * We have a FK between hypertable H and PAR. Hypertable H has number
		 * of chunks C1, C2, etc. When we execute "drop table C", PG acquires
		 * locks on C and PAR. If we have a query as "select * from
		 * hypertable", this acquires a lock on C and PAR as well. But the
		 * order of the locks is not the same and results in deadlocks. -
		 * github issue 865 We hope to alleviate the problem by acquiring a
		 * lock on PAR before executing the drop table stmt. This is not
		 * fool-proof as we could have multiple fkrelids and the order of lock
		 * acquisition for these could differ as well. Do not unlock - let the
		 * transaction semantics take care of it.
		 */
		foreach (lf, fk_relids)
		{
			LockRelationOid(lfirst_oid(lf), AccessExclusiveLock);
		}

		/* Drop chunks and store their names for return */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		dc_temp = ts_chunk_do_drop_chunks(table_relid,
										  older_than_datum,
										  newer_than_datum,
										  older_than_type,
										  newer_than_type,
										  cascade,
										  cascades_to_materializations,
										  elevel,
										  user_supplied_table_name,
										  &data_node_oids);

		dc_names = list_concat(dc_names, dc_temp);

		MemoryContextSwitchTo(oldcontext);
	}

	if (data_node_oids != NIL)
		drop_remote_chunks(fcinfo, schema_name, table_name, data_node_oids);

	/* store data for multi function call */
	funcctx->max_calls = list_length(dc_names);
	funcctx->user_fctx = dc_names;

	return list_return_srf(fcinfo);
}

/**
 * This function is used to explicitly specify chunks that are being scanned. It's being processed
 * in the planning phase and removed from the query tree. This means that the actual function
 * implementation will only be executed if something went wrong during explicit chunk exclusion.
 */
Datum
ts_chunks_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("illegal invocation of chunks_in function"),
			 errhint("chunks_in function must appear in the WHERE clause and can only be combined "
					 "with AND operator")));
	pg_unreachable();
}

/* Check if this chunk can be compressed, that it is not dropped and has not
 * already been compressed. */
bool
ts_chunk_can_be_compressed(int32 chunk_id)
{
	bool can_be_compressed = false;
	ScanIterator iterator = ts_scan_iterator_create(CHUNK, AccessShareLock, CurrentMemoryContext);
	iterator.ctx.index = catalog_get_index(ts_catalog_get(), CHUNK, CHUNK_ID_INDEX);
	ts_scan_iterator_scan_key_init(&iterator,
								   Anum_chunk_idx_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(chunk_id));

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		bool compressed_chunk_id_isnull, dropped_isnull;
		Datum dropped;
		heap_getattr(ti->tuple,
					 Anum_chunk_compressed_chunk_id,
					 ti->desc,
					 &compressed_chunk_id_isnull);
		dropped = heap_getattr(ti->tuple, Anum_chunk_dropped, ti->desc, &dropped_isnull);
		Assert(!dropped_isnull);
		can_be_compressed = compressed_chunk_id_isnull && !DatumGetBool(dropped);
	}
	ts_scan_iterator_close(&iterator);
	return can_be_compressed;
}

Datum
ts_chunk_dml_blocker(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const char *relname = get_rel_name(trigdata->tg_relation->rd_id);

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "dml_blocker: not called by trigger manager");
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("insert/update/delete not permitted on chunk \"%s\"", relname),
			 errhint("Make sure the chunk is not compressed.")));

	PG_RETURN_NULL();
}

Datum
ts_chunk_show(PG_FUNCTION_ARGS)
{
	return ts_cm_functions->show_chunk(fcinfo);
}

Datum
ts_chunk_create(PG_FUNCTION_ARGS)
{
	return ts_cm_functions->create_chunk(fcinfo);
}
