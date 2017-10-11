
/****************************************************************
 *
 * MODULE:       v.in.ogr
 *
 * AUTHOR(S):    Radim Blazek
 *               Markus Neteler (spatial parm, projection support)
 *               Paul Kelly (projection support)
 * 		 Markus Metz
 *
 * PURPOSE:      Import vector data with OGR
 *
 * COPYRIGHT:    (C) 2003-2016 by the GRASS Development Team
 *
 *               This program is free software under the GNU General
 *               Public License (>=v2).  Read the file COPYING that
 *               comes with GRASS for details.
 *
 * TODO: - make fixed field length of OFTIntegerList dynamic
 *       - several other TODOs below
**************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <grass/gis.h>
#include <grass/dbmi.h>
#include <grass/vector.h>
#include <grass/gprojects.h>
#include <grass/glocale.h>
#include <gdal_version.h>	/* needed for OFTDate */
#include "gdal.h"
#include "ogr_api.h"
#include "cpl_conv.h"
#include "global.h"

#ifndef MAX
#  define MIN(a,b)      ((a<b) ? a : b)
#  define MAX(a,b)      ((a>b) ? a : b)
#endif

/* define type of input datasource
 * as of GDAL 2.2, all functions having as argument a GDAL/OGR dataset 
 * must use the GDAL version, not the OGR version */
#if GDAL_VERSION_NUM >= 2020000
typedef GDALDatasetH ds_t;
#define ds_getlayerbyindex(ds, i)	GDALDatasetGetLayer((ds), (i))
#define ds_close(ds)			GDALClose(ds)
#else
typedef OGRDataSourceH ds_t;
#define ds_getlayerbyindex(ds, i)	OGR_DS_GetLayer((ds), (i))
#define ds_close(ds)			OGR_DS_Destroy(ds)
#endif

int n_polygons;
int n_polygon_boundaries;
double split_distance;

int geom(OGRGeometryH hGeom, struct Map_info *Map, int field, int cat,
	 double min_area, int type, int mk_centr);
int centroid(OGRGeometryH hGeom, CENTR * Centr, struct spatial_index * Sindex,
	     int field, int cat, double min_area, int type);
int poly_count(OGRGeometryH hGeom, int line2boundary);

char *get_datasource_name(const char *, int);

int cmp_layer_srs(ds_t, int, int *, char **, char *);
int get_layer_proj(OGRLayerH, struct Cell_head *,
		   struct Key_Value **, struct Key_Value **,
		   char *, int);
int create_spatial_filter(ds_t Ogr_ds, OGRGeometryH *,
                          int , int *, char **,
                          double *, double *,
			  double *, double *,
			  int , struct Option *);

struct OGR_iterator
{
    ds_t *Ogr_ds;
    char *dsn;
    int nlayers;
    int has_nonempty_layers;
    int ogr_interleaved_reading;
    OGRLayerH Ogr_layer;
    OGRFeatureDefnH Ogr_featuredefn;
    int requested_layer;
    int curr_layer;
    int done;
};

void OGR_iterator_init(struct OGR_iterator *OGR_iter,
                       ds_t *Ogr_ds, char *dsn, int nlayers,
		       int ogr_interleaved_reading);

void OGR_iterator_reset(struct OGR_iterator *OGR_iter);
OGRFeatureH ogr_getnextfeature(struct OGR_iterator *, int, char *,
			       OGRGeometryH , const char *);

int main(int argc, char *argv[])
{
    struct GModule *module;
    struct _param {
	struct Option *dsn, *out, *layer, *spat, *where,
	    *min_area;
        struct Option *snap, *type, *outloc, *cnames, *encoding, *key, *geom;
    } param;
    struct _flag {
	struct Flag *list, *no_clean, *force2d, *notab,
	    *region, *over, *extend, *formats, *tolower, *no_import,
            *proj;
    } flag;

    char *desc;

    int i, j, layer, nogeom, ncnames, igeom;
    double xmin, ymin, xmax, ymax;
    int ncols = 0, type;
    double min_area, snap;
    char buf[DB_SQL_MAX], namebuf[1024];
    char *separator;
    
    struct Key_Value *loc_proj_info, *loc_proj_units;
    struct Key_Value *proj_info, *proj_units;
    struct Cell_head cellhd, loc_wind, cur_wind;
    char error_msg[8192];

    /* Vector */
    struct Map_info Map, Tmp, *Out;
    int cat;
    int delete_table = FALSE; /* for external output format only */
    
    /* Attributes */
    struct field_info *Fi = NULL;
    dbDriver *driver = NULL;
    dbString sql, strval;
    int with_z, input3d;
    const char **key_column;
    int *key_idx;

    /* OGR */
    ds_t Ogr_ds;
    const char *ogr_driver_name;
    int ogr_interleaved_reading;
    OGRLayerH Ogr_layer;
    OGRFieldDefnH Ogr_field;
    char *Ogr_fieldname;
    OGRFieldType Ogr_ftype;
    OGRFeatureH Ogr_feature;
    OGRFeatureDefnH Ogr_featuredefn;
    OGRGeometryH Ogr_geometry, *poSpatialFilter;
    const char *attr_filter;
    struct OGR_iterator OGR_iter;
    int proj_trouble;

    int OFTIntegerListlength;

    char *dsn;
    const char *driver_name;
    const char *datetime_type;
    char *output;
    char **layer_names;		/* names of layers to be imported */
    int *layers;		/* layer indexes */
    int nlayers;		/* number of layers to import */
    char **available_layer_names;	/* names of layers to be imported */
    int navailable_layers;
    int layer_id;
    unsigned int feature_count;
    GIntBig *n_features, n_import_features;
    int overwrite;
    double area_size;
    int use_tmp_vect;
    int ncentr, n_overlaps;
    struct bound_box box;

    xmin = ymin = 1.0;
    xmax = ymax = 0.0;
    loc_proj_info = loc_proj_units = NULL;
    Ogr_ds = NULL;
    poSpatialFilter = NULL;
    OFTIntegerListlength = 255;	/* hack due to limitation in OGR */
    area_size = 0.0;
    use_tmp_vect = FALSE;

    G_gisinit(argv[0]);

    module = G_define_module();
    G_add_keyword(_("vector"));
    G_add_keyword(_("import"));
    G_add_keyword("OGR");
    module->description = _("Imports vector data into a GRASS vector map using OGR library.");

    param.dsn = G_define_option();
    param.dsn->key = "input";
    param.dsn->type = TYPE_STRING;
    param.dsn->required =YES;
    param.dsn->label = _("Name of OGR datasource to be imported");
    param.dsn->description = _("Examples:\n"
				   "\t\tESRI Shapefile: directory containing shapefiles\n"
				   "\t\tMapInfo File: directory containing mapinfo files");
    param.dsn->gisprompt = "old,datasource,datasource";
    
    param.layer = G_define_option();
    param.layer->key = "layer";
    param.layer->type = TYPE_STRING;
    param.layer->required = NO;
    param.layer->multiple = YES;
    param.layer->label =
	_("OGR layer name. If not given, all available layers are imported");
    param.layer->description =
	_("Examples:\n" "\t\tESRI Shapefile: shapefile name\n"
	  "\t\tMapInfo File: mapinfo file name");
    param.layer->guisection = _("Input");
    param.layer->gisprompt = "old,datasource_layer,datasource_layer";

    param.out = G_define_standard_option(G_OPT_V_OUTPUT);
    param.out->required = NO;
    param.out->guisection = _("Output");
    
    param.spat = G_define_option();
    param.spat->key = "spatial";
    param.spat->type = TYPE_DOUBLE;
    param.spat->multiple = YES;
    param.spat->required = NO;
    param.spat->key_desc = "xmin,ymin,xmax,ymax";
    param.spat->label = _("Import subregion only");
    param.spat->guisection = _("Selection");
    param.spat->description =
	_("Format: xmin,ymin,xmax,ymax - usually W,S,E,N");

    param.where = G_define_standard_option(G_OPT_DB_WHERE);
    param.where->guisection = _("Selection");

    param.min_area = G_define_option();
    param.min_area->key = "min_area";
    param.min_area->type = TYPE_DOUBLE;
    param.min_area->required = NO;
    param.min_area->answer = "0.0001";
    param.min_area->label =
	_("Minimum size of area to be imported (square meters)");
    param.min_area->guisection = _("Selection");
    param.min_area->description = _("Smaller areas and "
				  "islands are ignored. Should be greater than snap^2");

    param.type = G_define_standard_option(G_OPT_V_TYPE);
    param.type->options = "point,line,boundary,centroid";
    param.type->answer = "";
    param.type->description = _("Optionally change default input type");
    desc = NULL;
    G_asprintf(&desc,
	       "point;%s;line;%s;boundary;%s;centroid;%s",
	       _("import area centroids as points"),
	       _("import area boundaries as lines"),
	       _("import lines as area boundaries"),
	       _("import points as centroids"));
    param.type->descriptions = desc;
    param.type->guisection = _("Selection");

    param.snap = G_define_option();
    param.snap->key = "snap";
    param.snap->type = TYPE_DOUBLE;
    param.snap->required = NO;
    param.snap->answer = "-1";
    param.snap->label = _("Snapping threshold for boundaries (map units)");
    param.snap->description = _("'-1' for no snap");

    param.outloc = G_define_option();
    param.outloc->key = "location";
    param.outloc->type = TYPE_STRING;
    param.outloc->required = NO;
    param.outloc->description = _("Name for new location to create");
    param.outloc->key_desc = "name";
    param.outloc->guisection = _("Output");
    
    param.cnames = G_define_standard_option(G_OPT_DB_COLUMNS);
    param.cnames->description =
	_("List of column names to be used instead of original names, "
	  "first is used for category column");
    param.cnames->guisection = _("Attributes");

    param.encoding = G_define_option();
    param.encoding->key = "encoding";
    param.encoding->type = TYPE_STRING;
    param.encoding->required = NO;
    param.encoding->label =
        _("Encoding value for attribute data");
    param.encoding->description = 
        _("Overrides encoding interpretation, useful when importing ESRI Shapefile");
    param.encoding->guisection = _("Attributes");

    param.key = G_define_option();
    param.key->key = "key";
    param.key->type = TYPE_STRING;
    param.key->required = NO;
    param.key->label =
        _("Name of column used for categories");
    param.key->description = 
        _("If not given, categories are generated as unique values and stored in 'cat' column");
    param.key->guisection = _("Attributes");

    param.geom = G_define_standard_option(G_OPT_DB_COLUMN);
    param.geom->key = "geometry";
    param.geom->label = _("Name of geometry column");
    param.geom->description = _("If not given, all geometry columns from the input are used");
    param.geom->guisection = _("Selection");

    flag.formats = G_define_flag();
    flag.formats->key = 'f';
    flag.formats->description = _("List supported OGR formats and exit");
    flag.formats->guisection = _("Print");
    flag.formats->suppress_required = YES;

    flag.list = G_define_flag();
    flag.list->key = 'l';
    flag.list->description = _("List available OGR layers in data source and exit"); 
    flag.list->guisection = _("Print");
    flag.list->suppress_required = YES;

    /* if using -c, you lose topological information ! */
    flag.no_clean = G_define_flag();
    flag.no_clean->key = 'c';
    flag.no_clean->description = _("Do not clean polygons (not recommended)");
    flag.no_clean->guisection = _("Output");

    flag.force2d = G_define_flag();
    flag.force2d->key = '2';
    flag.force2d->label = _("Force 2D output even if input is 3D");
    flag.force2d->description = _("Useful if input is 3D but all z coordinates are identical");
    flag.force2d->guisection = _("Output");

    flag.notab = G_define_standard_flag(G_FLG_V_TABLE);
    flag.notab->guisection = _("Attributes");

    flag.over = G_define_flag();
    flag.over->key = 'o';
    flag.over->label =
	_("Override projection check (use current location's projection)");
    flag.over->description =
	_("Assume that the dataset has the same projection as the current location");

    flag.proj = G_define_flag();
    flag.proj->key = 'j';
    flag.proj->description =
	_("Perform projection check only and exit");
    flag.proj->suppress_required = YES;
    G_option_requires(flag.proj, param.dsn, NULL);
    
    flag.region = G_define_flag();
    flag.region->key = 'r';
    flag.region->guisection = _("Selection");
    flag.region->description = _("Limit import to the current region");

    flag.extend = G_define_flag();
    flag.extend->key = 'e';
    flag.extend->label =
	_("Extend region extents based on new dataset");
    flag.extend->description =
	_("Also updates the default region if in the PERMANENT mapset");

    flag.tolower = G_define_flag();
    flag.tolower->key = 'w';
    flag.tolower->description =
	_("Change column names to lowercase characters");
    flag.tolower->guisection = _("Attributes");

    flag.no_import = G_define_flag();
    flag.no_import->key = 'i';
    flag.no_import->description =
	_("Create the location specified by the \"location\" parameter and exit."
          " Do not import the vector data.");
    flag.no_import->guisection = _("Output");
    
    /* The parser checks if the map already exists in current mapset, this is
     * wrong if location options is used, so we switch out the check and do it
     * in the module after the parser */
    overwrite = G_check_overwrite(argc, argv);

    if (G_parser(argc, argv))
	exit(EXIT_FAILURE);

#if GDAL_VERSION_NUM >= 2000000
    GDALAllRegister();
#else
    OGRRegisterAll();
#endif

    G_debug(1, "GDAL version %d", GDAL_VERSION_NUM);

    /* list supported formats */
    if (flag.formats->answer) {
	int iDriver;

	G_message(_("Supported formats:"));

#if GDAL_VERSION_NUM >= 2000000
	for (iDriver = 0; iDriver < GDALGetDriverCount(); iDriver++) {
	    GDALDriverH hDriver = GDALGetDriver(iDriver);
	    const char *pszRWFlag;

            if (!GDALGetMetadataItem(hDriver, GDAL_DCAP_VECTOR, NULL))
		continue;

	    if (GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATE, NULL))
		pszRWFlag = "rw+";
	    else if (GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATECOPY, NULL))
		pszRWFlag = "rw";
	    else
		pszRWFlag = "ro";

	    fprintf(stdout, " %s (%s): %s\n",
		    GDALGetDriverShortName(hDriver),
		    pszRWFlag, GDALGetDriverLongName(hDriver));
	}

#else
	for (iDriver = 0; iDriver < OGRGetDriverCount(); iDriver++) {
	    OGRSFDriverH poDriver = OGRGetDriver(iDriver);
	    const char *pszRWFlag;
	    
	    if (OGR_Dr_TestCapability(poDriver, ODrCCreateDataSource))
		pszRWFlag = "rw";
	    else
		pszRWFlag = "ro";

	    fprintf(stdout, " %s (%s): %s\n",
		    OGR_Dr_GetName(poDriver),
		    pszRWFlag, OGR_Dr_GetName(poDriver));
	}
#endif
	exit(EXIT_SUCCESS);
    }

    if (param.dsn->answer == NULL) {
	G_fatal_error(_("Required parameter <%s> not set"), param.dsn->key);
    }

    driver_name = db_get_default_driver_name();

    if (driver_name && strcmp(driver_name, "pg") == 0)
	datetime_type = "timestamp";
    else if (driver_name && strcmp(driver_name, "dbf") == 0)
	datetime_type = "varchar(22)";
    else
	datetime_type = "datetime";

    dsn = NULL;
    if (param.dsn->answer)
        dsn = get_datasource_name(param.dsn->answer, TRUE);
    
    min_area = atof(param.min_area->answer);
    snap = atof(param.snap->answer);
    type = Vect_option_to_types(param.type);

    ncnames = 0;
    if (param.cnames->answers) {
	i = 0;
	while (param.cnames->answers[i]) {
	    G_strip(param.cnames->answers[i]);
	    G_strchg(param.cnames->answers[i], ' ', '\0');
	    ncnames++;
	    i++;
	}
    }

    /* set up encoding for attribute data */
    if (param.encoding->answer) {
	char *encbuf, *encp;
	int len;
	
	len = strlen("SHAPE_ENCODING") + strlen(param.encoding->answer) + 2;
	encbuf = G_malloc(len * sizeof(char));
        /* -> Esri Shapefile */
	sprintf(encbuf, "SHAPE_ENCODING=%s", param.encoding->answer);
	encp = G_store(encbuf);
	putenv(encp);
        /* -> DXF */
	sprintf(encbuf, "DXF_ENCODING=%s", param.encoding->answer);
	encp = G_store(encbuf);
	putenv(encp);
        /* todo: others ? */
	G_free(encbuf);
    }

    /* open OGR DSN */
    Ogr_ds = NULL;
    if (strlen(dsn) > 0) {
#if GDAL_VERSION_NUM >= 2020000
	Ogr_ds = GDALOpenEx(dsn, GDAL_OF_VECTOR, NULL, NULL, NULL);
#else
	Ogr_ds = OGROpen(dsn, FALSE, NULL);
#endif
    }
    if (Ogr_ds == NULL)
	G_fatal_error(_("Unable to open data source <%s>"), dsn);

    /* driver name */
#if GDAL_VERSION_NUM >= 2020000
    ogr_driver_name = GDALGetDriverShortName(GDALGetDatasetDriver(Ogr_ds));
    G_verbose_message(_("Using OGR driver '%s/%s'"), ogr_driver_name,
                      GDALGetDriverLongName(GDALGetDatasetDriver(Ogr_ds)));
#else
    ogr_driver_name = OGR_Dr_GetName(OGR_DS_GetDriver(Ogr_ds));
    G_verbose_message(_("Using OGR driver '%s'"), ogr_driver_name);
#endif

    /* OGR interleaved reading */
    ogr_interleaved_reading = 0;
    if (strcmp(ogr_driver_name, "OSM") == 0) {

	/* re-open OGR DSN */
#if GDAL_VERSION_NUM < 2020000
	CPLSetConfigOption("OGR_INTERLEAVED_READING", "YES");
	OGR_DS_Destroy(Ogr_ds);
	Ogr_ds = OGROpen(dsn, FALSE, NULL);
#endif
	ogr_interleaved_reading = 1;
    }
    if (strcmp(ogr_driver_name, "GMLAS") == 0)
	ogr_interleaved_reading = 1;
    if (ogr_interleaved_reading)
	G_verbose_message(_("Using interleaved reading mode"));

    if (param.geom->answer) {
#if GDAL_VERSION_NUM >= 1110000
#if GDAL_VERSION_NUM >= 2020000
        if (!GDALDatasetTestCapability(Ogr_ds, ODsCCreateGeomFieldAfterCreateLayer)) {
            G_warning(_("Option <%s> will be ignored. OGR doesn't support it for selected format (%s)."),
                      param.geom->key, ogr_driver_name);
#else
        if (!OGR_DS_TestCapability(Ogr_ds, ODsCCreateGeomFieldAfterCreateLayer)) {
            G_warning(_("Option <%s> will be ignored. OGR doesn't support it for selected format (%s)."),
                      param.geom->key, ogr_driver_name);
#endif
            param.geom->answer = NULL;
        }
#else
        G_warning(_("Option <%s> will be ignored. Multiple geometry fields are supported by GDAL >= 1.11"),
                  param.geom->key);
        param.geom->answer = NULL;
#endif
    }

    /* check encoding for given driver */
    if (param.encoding->answer) {
        if (strcmp(ogr_driver_name, "ESRI Shapefile") != 0 &&
            strcmp(ogr_driver_name, "DXF") != 0)
            G_warning(_("Encoding value not supported by OGR driver <%s>"), ogr_driver_name);
    }

#if GDAL_VERSION_NUM >= 2020000
    navailable_layers = GDALDatasetGetLayerCount(Ogr_ds);
#else
    navailable_layers = OGR_DS_GetLayerCount(Ogr_ds);
#endif

    if (navailable_layers < 1)
	G_fatal_error(_("No OGR layers available"));

    /* make a list of available layers */
    available_layer_names =
	(char **)G_malloc(navailable_layers * sizeof(char *));

    if (flag.list->answer) {
	G_message(_("Data source <%s> (format '%s') contains %d layers:"),
		  dsn, ogr_driver_name, navailable_layers);
    }
    for (i = 0; i < navailable_layers; i++) {
	Ogr_layer = ds_getlayerbyindex(Ogr_ds, i);
	Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);
        
	available_layer_names[i] =
	    G_store((char *)OGR_FD_GetName(Ogr_featuredefn));

	if (flag.list->answer)
	    fprintf(stdout, "%s\n", available_layer_names[i]);
    }
    if (flag.list->answer) {
	fflush(stdout);
	ds_close(Ogr_ds);
	exit(EXIT_SUCCESS);
    }
    
    /* Make a list of layers to be imported */
    if (param.layer->answer) {	/* From option */
	nlayers = 0;
	while (param.layer->answers[nlayers])
	    nlayers++;

	layer_names = (char **)G_malloc(nlayers * sizeof(char *));
	layers = (int *)G_malloc(nlayers * sizeof(int));

	for (i = 0; i < nlayers; i++) {
	    layer_names[i] = G_store(param.layer->answers[i]);
	    /* Find it in the source */
	    layers[i] = -1;
	    for (j = 0; j < navailable_layers; j++) {
		if (strcmp(available_layer_names[j], layer_names[i]) == 0) {
		    layers[i] = j;
		    break;
		}
	    }
	    if (layers[i] == -1)
		G_fatal_error(_("Layer <%s> not available"), layer_names[i]);
	}
    }
    else {			/* use list of all layers */
	nlayers = navailable_layers;
	layer_names = available_layer_names;
	layers = (int *)G_malloc(nlayers * sizeof(int));
	for (i = 0; i < nlayers; i++)
	    layers[i] = i;
    }

    /* compare SRS of the different layers to be imported */
    if (cmp_layer_srs(Ogr_ds, nlayers, layers, layer_names, param.geom->answer)) {
	ds_close(Ogr_ds);
	G_fatal_error(_("Detected different projections of input layers. "
	                "Input layers must be imported separately."));
    }

    /* Get first imported layer to use for projection check */
    Ogr_layer = ds_getlayerbyindex(Ogr_ds, layers[0]);

    /* Fetch input projection in GRASS form. */
    proj_info = NULL;
    proj_units = NULL;
    G_get_window(&cellhd);

    proj_trouble = get_layer_proj(Ogr_layer, &cellhd, &proj_info, &proj_units,
		   param.geom->answer, 1);

    cellhd.north = 1.;
    cellhd.south = 0.;
    cellhd.west = 0.;
    cellhd.east = 1.;
    cellhd.top = 1.;
    cellhd.bottom = 0.;
    cellhd.rows = 1;
    cellhd.rows3 = 1;
    cellhd.cols = 1;
    cellhd.cols3 = 1;
    cellhd.depths = 1;
    cellhd.ns_res = 1.;
    cellhd.ns_res3 = 1.;
    cellhd.ew_res = 1.;
    cellhd.ew_res3 = 1.;
    cellhd.tb_res = 1.;

    /* Do we need to create a new location? */
    if (param.outloc->answer != NULL) {
	/* Convert projection information non-interactively as we can't
	 * assume the user has a terminal open */

	/* do not create a xy location because this can mean that the
	 * real SRS has not been recognized */ 
	if (proj_trouble) {
	    G_fatal_error(_("Unable to convert input map projection to GRASS "
			    "format; cannot create new location."));
	}
	else {
            if (0 != G_make_location(param.outloc->answer, &cellhd,
                                     proj_info, proj_units)) {
                G_fatal_error(_("Unable to create new location <%s>"),
                              param.outloc->answer);
            }
	    G_message(_("Location <%s> created"), param.outloc->answer);

	    G_unset_window();	/* new location, projection, and window */
	    G_get_window(&cellhd);
	}

        /* If the i flag is set, clean up and exit here */
        if (flag.no_import->answer) {
	    ds_close(Ogr_ds);
            exit(EXIT_SUCCESS);
        }
    }
    else {
	int err = 0;
        void (*msg_fn)(const char *, ...);
            
	/* Projection only required for checking so convert non-interactively */
	if (proj_trouble) {
	    strcpy(error_msg, _("Unable to convert input map projection information "
		                "to GRASS format."));
            if (flag.over->answer) {
                msg_fn = G_warning;
	    }
            else {
                msg_fn = G_fatal_error;
		ds_close(Ogr_ds);
	    }
            msg_fn(error_msg);
            if (!flag.over->answer) {
                exit(EXIT_FAILURE);
	    }
	}

	/* Does the projection of the current location match the dataset? */
	/* G_get_window seems to be unreliable if the location has been changed */
	G_get_default_window(&loc_wind);
	/* fetch LOCATION PROJ info */
	if (loc_wind.proj != PROJECTION_XY) {
	    loc_proj_info = G_get_projinfo();
	    loc_proj_units = G_get_projunits();
	}

	if (flag.over->answer) {
	    cellhd.proj = loc_wind.proj;
	    cellhd.zone = loc_wind.zone;
	    G_message(_("Over-riding projection check"));
	}
	else if (loc_wind.proj != cellhd.proj
		 || (err =
		     G_compare_projections(loc_proj_info, loc_proj_units,
					   proj_info, proj_units)) != TRUE) {
	    int i_value;

	    strcpy(error_msg,
		   _("Projection of dataset does not"
		     " appear to match current location.\n\n"));

	    /* TODO: output this info sorted by key: */
	    if (loc_wind.proj != cellhd.proj || err != -2) {
		if (loc_proj_info != NULL) {
		    strcat(error_msg, _("GRASS LOCATION PROJ_INFO is:\n"));
		    for (i_value = 0; i_value < loc_proj_info->nitems;
			 i_value++)
			sprintf(error_msg + strlen(error_msg), "%s: %s\n",
				loc_proj_info->key[i_value],
				loc_proj_info->value[i_value]);
		    strcat(error_msg, "\n");
		}

		if (proj_info != NULL) {
		    strcat(error_msg, _("Import dataset PROJ_INFO is:\n"));
		    for (i_value = 0; i_value < proj_info->nitems; i_value++)
			sprintf(error_msg + strlen(error_msg), "%s: %s\n",
				proj_info->key[i_value],
				proj_info->value[i_value]);
		}
		else {
		    strcat(error_msg, _("Import dataset PROJ_INFO is:\n"));
		    if (cellhd.proj == PROJECTION_XY)
			sprintf(error_msg + strlen(error_msg),
				"Dataset proj = %d (unreferenced/unknown)\n",
				cellhd.proj);
		    else if (cellhd.proj == PROJECTION_LL)
			sprintf(error_msg + strlen(error_msg),
				"Dataset proj = %d (lat/long)\n",
				cellhd.proj);
		    else if (cellhd.proj == PROJECTION_UTM)
			sprintf(error_msg + strlen(error_msg),
				"Dataset proj = %d (UTM), zone = %d\n",
				cellhd.proj, cellhd.zone);
		    else
			sprintf(error_msg + strlen(error_msg),
				"Dataset proj = %d (unknown), zone = %d\n",
				cellhd.proj, cellhd.zone);
		}
	    }
	    else {
		if (loc_proj_units != NULL) {
		    strcat(error_msg, "GRASS LOCATION PROJ_UNITS is:\n");
		    for (i_value = 0; i_value < loc_proj_units->nitems;
			 i_value++)
			sprintf(error_msg + strlen(error_msg), "%s: %s\n",
				loc_proj_units->key[i_value],
				loc_proj_units->value[i_value]);
		    strcat(error_msg, "\n");
		}

		if (proj_units != NULL) {
		    strcat(error_msg, "Import dataset PROJ_UNITS is:\n");
		    for (i_value = 0; i_value < proj_units->nitems; i_value++)
			sprintf(error_msg + strlen(error_msg), "%s: %s\n",
				proj_units->key[i_value],
				proj_units->value[i_value]);
		}
	    }
	    sprintf(error_msg + strlen(error_msg),
		    _("\nIn case of no significant differences in the projection definitions,"
		      " use the -o flag to ignore them and use"
		      " current location definition.\n"));
	    strcat(error_msg,
		   _("Consider generating a new location with 'location' parameter"
		    " from input data set.\n"));
            if (flag.proj->answer)
                msg_fn = G_message;
            else {
                msg_fn = G_fatal_error;
		ds_close(Ogr_ds);
	    }
            msg_fn(error_msg);
            if (flag.proj->answer)
                exit(EXIT_FAILURE);
	}
	else {
	    if (flag.proj->answer)
		msg_fn = G_message;
	    else
		msg_fn = G_verbose_message;            
	    msg_fn(_("Projection of input dataset and current location "
		     "appear to match"));
	    if (flag.proj->answer) {
		ds_close(Ogr_ds);
		exit(EXIT_SUCCESS);
	    }
	}
    }

    /* get output name */
    if (param.out->answer) {
	output = G_store(param.out->answer);
    }
    else {
	output = G_store(layer_names[0]);
    }

    /* check output name */
    if (Vect_legal_filename(output) != 1) {
	ds_close(Ogr_ds);
	G_fatal_error(_("Illegal output name <%s>"), output);
    }

    /* Check if the output map exists */
    if (G_find_vector2(output, G_mapset()) && !overwrite) {
	ds_close(Ogr_ds);
	G_fatal_error(_("Vector map <%s> already exists"),
		      output);
    }

    /* report back if several layers will be imported because 
     * none has been selected */
    if (nlayers > 1 && param.layer->answer == NULL) {
	void (*msg_fn)(const char *, ...);

	/* make it a warning if output name has not been specified */
	if (param.out->answer)
	    msg_fn = G_important_message;
	else
	    msg_fn = G_warning;

	msg_fn(_("All available OGR layers will be imported into vector map <%s>"),
		  output);
    }

    /* attribute filter */
    attr_filter = param.where->answer;

    /* create spatial filters */
    if (param.outloc->answer && flag.region->answer) {
	G_warning(_("When creating a new location, the current region "
	          "can not be used as spatial filter, disabling"));
	flag.region->answer = 0;
    }
    if (flag.region->answer && param.spat->answer)
	G_fatal_error(_("Select either the current region flag or the spatial option, not both"));

    poSpatialFilter = G_malloc(nlayers * sizeof(OGRGeometryH));
    if (create_spatial_filter(Ogr_ds, poSpatialFilter,
                              nlayers, layers, layer_names,
			      &xmin, &ymin, &xmax, &ymax,
			      flag.region->answer, param.spat)
	|| attr_filter) {

	for (layer = 0; layer < nlayers; layer++) {
	    Ogr_layer = ds_getlayerbyindex(Ogr_ds, layers[layer]);
	    OGR_L_SetSpatialFilter(Ogr_layer, poSpatialFilter[layer]);
	    if (OGR_L_SetAttributeFilter(Ogr_layer, attr_filter) != OGRERR_NONE)
		G_fatal_error(_("Error setting attribute filter '%s'"),
			      attr_filter);
	}
    }

    /* suppress boundary splitting ? */
    if (flag.no_clean->answer || xmin >= xmax || ymin >= ymax) {
	split_distance = -1.;
	area_size = -1;
    }
    else {
	split_distance = 0.;
	area_size = sqrt((xmax - xmin) * (ymax - ymin));
    }

    db_init_string(&sql);
    db_init_string(&strval);

    n_features = (GIntBig *)G_malloc(nlayers * sizeof(GIntBig));

    OGR_iterator_init(&OGR_iter, &Ogr_ds, dsn, navailable_layers,
		      ogr_interleaved_reading);

    /* check if input id 3D and if we need a tmp vector */
    /* estimate distance for boundary splitting --> */
    n_polygon_boundaries = 0;
    input3d = 0;

    for (layer = 0; layer < nlayers; layer++) {
	GIntBig ogr_feature_count;

	n_features[layer] = 0;
	layer_id = layers[layer];
	Ogr_layer = ds_getlayerbyindex(Ogr_ds, layer_id);
	Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);
        igeom = -1;
#if GDAL_VERSION_NUM >= 1110000
        if (param.geom->answer) {
            igeom = OGR_FD_GetGeomFieldIndex(Ogr_featuredefn, param.geom->answer);
            if (igeom < 0)
                G_fatal_error(_("Geometry column <%s> not found in OGR layer <%s>"),
                              param.geom->answer, OGR_L_GetName(Ogr_layer));
        }
#endif
	feature_count = 0;

	ogr_feature_count = 0;
	if (n_features[layer_id] == 0)
	    ogr_feature_count = OGR_L_GetFeatureCount(Ogr_layer, 1);
	if (ogr_feature_count > 0)
	    n_features[layer_id] = ogr_feature_count;

	/* count polygons and isles */
	G_message(_("Check if OGR layer <%s> contains polygons..."),
		  layer_names[layer]);
	while ((Ogr_feature = ogr_getnextfeature(&OGR_iter, layer_id,
	                                         layer_names[layer],
						 poSpatialFilter[layer],
						 attr_filter)) != NULL) {
	    if (ogr_feature_count > 0)
		G_percent(feature_count++, n_features[layer], 1);	/* show something happens */

	    if (ogr_feature_count <= 0)
		n_features[layer]++;

            /* Geometry */
#if GDAL_VERSION_NUM >= 1110000
            Ogr_featuredefn = OGR_iter.Ogr_featuredefn;
            for (i = 0; i < OGR_FD_GetGeomFieldCount(Ogr_featuredefn); i++) {
                if (igeom > -1 && i != igeom)
                    continue; /* use only geometry defined via param.geom */
            
                Ogr_geometry = OGR_F_GetGeomFieldRef(Ogr_feature, i);
#else
                Ogr_geometry = OGR_F_GetGeometryRef(Ogr_feature);
#endif
                if (Ogr_geometry != NULL) {
#if GDAL_VERSION_NUM >= 2000000
		    Ogr_geometry = OGR_G_GetLinearGeometry(Ogr_geometry, 0, NULL);
		}
                if (Ogr_geometry != NULL) {
#endif
                    if (!flag.no_clean->answer)
                        poly_count(Ogr_geometry, (type & GV_BOUNDARY));
                    if (OGR_G_GetCoordinateDimension(Ogr_geometry) > 2)
                        input3d = 1;
#if GDAL_VERSION_NUM >= 2000000
		    OGR_G_DestroyGeometry(Ogr_geometry);
#endif
                }
#if GDAL_VERSION_NUM >= 1110000                
            }
#endif
            OGR_F_Destroy(Ogr_feature);
	}
	G_percent(1, 1, 1);
    }

    n_import_features = 0;
    for (i = 0; i < nlayers; i++)
	n_import_features += n_features[i];
    if (nlayers > 1)
	G_message("Importing %lld features", n_import_features);

    G_debug(1, "n polygon boundaries: %d", n_polygon_boundaries);
    if (area_size > 0 && n_polygon_boundaries > 50) {
	split_distance =
	    area_size / log(n_polygon_boundaries);
	/* divisor is the handle: increase divisor to decrease split_distance */
	split_distance = split_distance / 16.;
	G_debug(1, "root of area size: %f", area_size);
	G_verbose_message(_("Boundary splitting distance in map units: %G"),
		  split_distance);
    }
    /* <-- estimate distance for boundary splitting */

    use_tmp_vect = n_polygon_boundaries > 0;

    G_debug(1, "Input is 3D ? %s", (input3d == 0 ? "yes" : "no"));
    with_z = input3d;
    if (with_z)
	with_z = !flag.force2d->answer;

    /* open output vector */
    /* strip any @mapset from vector output name */
    G_find_vector(output, G_mapset());

    if (Vect_open_new(&Map, output, with_z) < 0)
	G_fatal_error(_("Unable to create vector map <%s>"), output);

    Out = &Map;

    if (!flag.no_clean->answer) {
	if (use_tmp_vect) {
	    /* open temporary vector, do the work in the temporary vector
	     * at the end copy alive lines to output vector
	     * in case of polygons this reduces the coor file size by a factor of 2 to 5
	     * only needed when cleaning polygons */
	    if (Vect_open_tmp_new(&Tmp, NULL, with_z) < 0)
		G_fatal_error(_("Unable to create temporary vector map"));

	    G_verbose_message(_("Using temporary vector <%s>"), Vect_get_name(&Tmp));
	    Out = &Tmp;
	}
    }

    Vect_hist_command(&Map);

    ncentr = n_overlaps = n_polygons = 0;

    G_begin_polygon_area_calculations();	/* Used in geom() and centroid() */

    /* Points and lines are written immediately with categories. Boundaries of polygons are
     * written to the vector then cleaned and centroids are calculated for all areas in clean vector.
     * Then second pass through finds all centroids in each polygon feature and adds its category
     * to the centroid. The result is that one centroid may have 0, 1 ore more categories
     * of one ore more (more input layers) fields. */

    /* get input column to use for categoy values, create tables */
    OGR_iterator_reset(&OGR_iter);
    key_column = G_malloc(nlayers * sizeof(char *));
    key_idx = G_malloc(nlayers * sizeof(int));
    for (layer = 0; layer < nlayers; layer++) {

	key_column[layer] = GV_KEY_COLUMN;
	key_idx[layer] = -2; /* -1 for fid column */
	layer_id = layers[layer];
	Ogr_layer = ds_getlayerbyindex(Ogr_ds, layer_id);
	Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);

        if (param.key->answer) {
	    /* use existing column for category values */
            const char *fid_column;

            fid_column = OGR_L_GetFIDColumn(Ogr_layer);
            if (fid_column) {
                key_column[layer] = G_store(fid_column);
                key_idx[layer] = -1;
            }
            if (!fid_column || strcmp(fid_column, param.key->answer) != 0) {
                key_idx[layer] = OGR_FD_GetFieldIndex(Ogr_featuredefn, param.key->answer);
                if (key_idx[layer] == -1)
                    G_fatal_error(_("Key column '%s' not found in input layer <%s>"),
		                  param.key->answer, layer_names[layer]);
            }

            if (key_idx[layer] > -1) {
                /* check if the field is integer */
                Ogr_field = OGR_FD_GetFieldDefn(Ogr_featuredefn, key_idx[layer]);
                Ogr_ftype = OGR_Fld_GetType(Ogr_field);
                if (!(Ogr_ftype == OFTInteger
#if GDAL_VERSION_NUM >= 2000000
                      || Ogr_ftype == OFTInteger64
#endif
		      )) {
                    G_fatal_error(_("Key column '%s' in input layer <%s> is not integer"),
		                  param.key->answer, layer_names[layer]);
                }
                key_column[layer] = G_store(OGR_Fld_GetNameRef(Ogr_field));
            }
        }

	/* Add DB link and create table */
	if (!flag.notab->answer) {
	    G_important_message(_("Creating attribute table for layer <%s>..."),
				  layer_names[layer]);

	    if (nlayers == 1) {	/* one layer only */
		Fi = Vect_default_field_info(&Map, layer + 1, NULL,
					     GV_1TABLE);
	    }
	    else {
		Fi = Vect_default_field_info(&Map, layer + 1, NULL,
					     GV_MTABLE);
	    }

	    if (ncnames > 0) {
		key_column[layer] = param.cnames->answers[0];
	    }
	    Vect_map_add_dblink(&Map, layer + 1, layer_names[layer], Fi->table,
				key_column[layer], Fi->database, Fi->driver);

	    ncols = OGR_FD_GetFieldCount(Ogr_featuredefn);
	    G_debug(2, "%d columns", ncols);

	    /* Create table */
	    sprintf(buf, "create table %s (%s integer", Fi->table,
		    key_column[layer]);
	    db_set_string(&sql, buf);
	    for (i = 0; i < ncols; i++) {

                if (key_idx[layer] > -1 && key_idx[layer] == i)
                    continue; /* skip defined key (FID column) */
                
		Ogr_field = OGR_FD_GetFieldDefn(Ogr_featuredefn, i);
		Ogr_ftype = OGR_Fld_GetType(Ogr_field);

		G_debug(3, "Ogr_ftype: %i", Ogr_ftype);	/* look up below */

		if (i < ncnames - 1) {
		    Ogr_fieldname = G_store(param.cnames->answers[i + 1]);
		}
		else {
		    /* Change column names to [A-Za-z][A-Za-z0-9_]* */
		    Ogr_fieldname = G_store(OGR_Fld_GetNameRef(Ogr_field));
		    G_debug(3, "Ogr_fieldname: '%s'", Ogr_fieldname);

		    G_str_to_sql(Ogr_fieldname);

		    G_debug(3, "Ogr_fieldname: '%s'", Ogr_fieldname);

		}

		/* avoid that we get the 'cat' column twice */
		if (strcmp(Ogr_fieldname, GV_KEY_COLUMN) == 0) {
		    sprintf(namebuf, "%s_", Ogr_fieldname);
		    Ogr_fieldname = G_store(namebuf);
		}

		/* capital column names are a pain in SQL */
		if (flag.tolower->answer)
		    G_str_to_lower(Ogr_fieldname);

		if (strcmp(OGR_Fld_GetNameRef(Ogr_field), Ogr_fieldname) != 0) {
		    G_important_message(_("Column name <%s> renamed to <%s>"),
			      OGR_Fld_GetNameRef(Ogr_field), Ogr_fieldname);
		}

		/** Simple 32bit integer                     OFTInteger = 0        **/
		/** List of 32bit integers                   OFTIntegerList = 1    **/
		/** Double Precision floating point          OFTReal = 2           **/
		/** List of doubles                          OFTRealList = 3       **/
		/** String of ASCII chars                    OFTString = 4         **/
		/** Array of strings                         OFTStringList = 5     **/
		/** Double byte string (unsupported)         OFTWideString = 6     **/
		/** List of wide strings (unsupported)       OFTWideStringList = 7 **/
		/** Raw Binary data (unsupported)            OFTBinary = 8         **/
		/**                                          OFTDate = 9           **/
		/**                                          OFTTime = 10          **/
		/**                                          OFTDateTime = 11      **/
                /** GDAL 2.0+                                                      **/
                /** Simple 64bit integer                     OFTInteger64 = 12     **/
                /** List of 64bit integers                   OFTInteger64List = 13 **/

		if (Ogr_ftype == OFTInteger) {
		    sprintf(buf, ", %s integer", Ogr_fieldname);
		}
#if GDAL_VERSION_NUM >= 2000000
		else if (Ogr_ftype == OFTInteger64) {
                    if (strcmp(Fi->driver, "pg") == 0) 
                        sprintf(buf, ", %s bigint", Ogr_fieldname);
                    else {
                        sprintf(buf, ", %s integer", Ogr_fieldname);
                        if (strcmp(Fi->driver, "sqlite") != 0) 
                            G_warning(_("Writing column <%s> with integer 64 as integer 32"),
                                      Ogr_fieldname);
                    }
                }
#endif
		else if (Ogr_ftype == OFTIntegerList
#if GDAL_VERSION_NUM >= 2000000
                         || Ogr_ftype == OFTInteger64List
#endif
                         ) {
		    /* hack: treat as string */
		    sprintf(buf, ", %s varchar ( %d )", Ogr_fieldname,
			    OFTIntegerListlength);
		    G_warning(_("Writing column <%s> with fixed length %d chars (may be truncated)"),
			      Ogr_fieldname, OFTIntegerListlength);
		}
		else if (Ogr_ftype == OFTReal) {
		    sprintf(buf, ", %s double precision", Ogr_fieldname);
#if GDAL_VERSION_NUM >= 1320
		}
		else if (Ogr_ftype == OFTDate) {
		    sprintf(buf, ", %s date", Ogr_fieldname);
		}
		else if (Ogr_ftype == OFTTime) {
		    sprintf(buf, ", %s time", Ogr_fieldname);
		}
		else if (Ogr_ftype == OFTDateTime) {
		    sprintf(buf, ", %s %s", Ogr_fieldname, datetime_type);
#endif
		}
		else if (Ogr_ftype == OFTString) {
		    int fwidth;

		    fwidth = OGR_Fld_GetWidth(Ogr_field);
		    /* TODO: read all records first and find the longest string length */
		    if (fwidth == 0) {
			G_warning(_("Width for column %s set to 255 (was not specified by OGR), "
				   "some strings may be truncated!"),
				  Ogr_fieldname);
			fwidth = 255;
		    }
		    sprintf(buf, ", %s varchar ( %d )", Ogr_fieldname,
			    fwidth);
		}
		else if (Ogr_ftype == OFTStringList) {
		    /* hack: treat as string */
		    sprintf(buf, ", %s varchar ( %d )", Ogr_fieldname,
			    OFTIntegerListlength);
		    G_warning(_("Writing column %s with fixed length %d chars (may be truncated)"),
			      Ogr_fieldname, OFTIntegerListlength);
		}
		else {
		    G_warning(_("Column type (Ogr_ftype: %d) not supported (Ogr_fieldname: %s)"),
			      Ogr_ftype, Ogr_fieldname);
		    buf[0] = 0;
		}
		db_append_string(&sql, buf);
		G_free(Ogr_fieldname);
	    }
	    db_append_string(&sql, ")");
	    G_debug(3, "%s", db_get_string(&sql));

	    driver =
		db_start_driver_open_database(Fi->driver,
					      Vect_subst_var(Fi->database,
							     &Map));
	    if (driver == NULL) {
		G_fatal_error(_("Unable to open database <%s> by driver <%s>"),
			      Vect_subst_var(Fi->database, &Map), Fi->driver);
	    }

	    if (db_execute_immediate(driver, &sql) != DB_OK) {
		db_close_database(driver);
		db_shutdown_driver(driver);
		G_fatal_error(_("Unable to create table: '%s'"),
			      db_get_string(&sql));
	    }

	    if (db_grant_on_table
		(driver, Fi->table, DB_PRIV_SELECT,
		 DB_GROUP | DB_PUBLIC) != DB_OK)
		G_fatal_error(_("Unable to grant privileges on table <%s>"),
			      Fi->table);

	    db_close_database_shutdown_driver(driver);
	}
    }

    /* import features */
    OGR_iterator_reset(&OGR_iter);
    for (layer = 0; layer < nlayers; layer++) {
	layer_id = layers[layer];
	/* Import features */
	cat = 1;
	nogeom = 0;
	feature_count = 0;

	G_important_message(_("Importing %lld features (OGR layer <%s>)..."),
			    n_features[layer], layer_names[layer]);

	driver = NULL;
	if (!flag.notab->answer) {
	    /* one transaction per layer/table
	     * or better one transaction for all layers/tables together ?
	     */
	    Fi = Vect_get_field(&Map, layer + 1);
	    driver =
		db_start_driver_open_database(Fi->driver,
					      Vect_subst_var(Fi->database,
							     &Map));
	    if (driver == NULL) {
		G_fatal_error(_("Unable to open database <%s> by driver <%s>"),
			      Vect_subst_var(Fi->database, &Map), Fi->driver);
	    }
	    db_begin_transaction(driver);
	}

	Ogr_layer = ds_getlayerbyindex(Ogr_ds, layer_id);
	Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);

        igeom = -1;
#if GDAL_VERSION_NUM >= 1110000
        if (param.geom->answer)
            igeom = OGR_FD_GetGeomFieldIndex(Ogr_featuredefn, param.geom->answer);
#endif

	while ((Ogr_feature = ogr_getnextfeature(&OGR_iter, layer_id,
	                                         layer_names[layer],
						 poSpatialFilter[layer],
						 attr_filter)) != NULL) {
	    G_percent(feature_count++, n_features[layer], 1);	/* show something happens */

            /* Geometry */
            Ogr_featuredefn = OGR_iter.Ogr_featuredefn;
#if GDAL_VERSION_NUM >= 1110000
            for (i = 0; i < OGR_FD_GetGeomFieldCount(Ogr_featuredefn); i++) {
                if (igeom > -1 && i != igeom)
                    continue; /* use only geometry defined via param.geom */
            
                Ogr_geometry = OGR_F_GetGeomFieldRef(Ogr_feature, i);
#else
                Ogr_geometry = OGR_F_GetGeometryRef(Ogr_feature);
#endif                
#if GDAL_VERSION_NUM >= 2000000
                if (Ogr_geometry != NULL) {
		    if (OGR_G_HasCurveGeometry(Ogr_geometry, 1)) {
			G_debug(2, "Approximating curves in a '%s'",
			        OGR_G_GetGeometryName(Ogr_geometry));
		    }
		    Ogr_geometry = OGR_G_GetLinearGeometry(Ogr_geometry, 0, NULL);
		}
#endif
                if (Ogr_geometry == NULL) {
                    nogeom++;
                }
                else {
                    if (key_idx[layer] > -1)
                        cat = OGR_F_GetFieldAsInteger(Ogr_feature, key_idx[layer]);
                    else if (key_idx[layer] == -1)
                        cat = OGR_F_GetFID(Ogr_feature);

                    geom(Ogr_geometry, Out, layer + 1, cat, min_area, type,
                         flag.no_clean->answer);
#if GDAL_VERSION_NUM >= 2000000
		    OGR_G_DestroyGeometry(Ogr_geometry);
#endif
                }
#if GDAL_VERSION_NUM >= 1110000              
            }
#endif
	    /* Attributes */
	    ncols = OGR_FD_GetFieldCount(Ogr_featuredefn);
	    if (!flag.notab->answer) {
		sprintf(buf, "insert into %s values ( %d", Fi->table, cat);
		db_set_string(&sql, buf);
		for (i = 0; i < ncols; i++) {
		    const char *Ogr_fstring = NULL;

                    if (key_idx[layer] > -1 && key_idx[layer] == i)
                        continue; /* skip defined key (FID column) */

		    Ogr_field = OGR_FD_GetFieldDefn(Ogr_featuredefn, i);
		    Ogr_ftype = OGR_Fld_GetType(Ogr_field);
		    if (OGR_F_IsFieldSet(Ogr_feature, i))
			Ogr_fstring = OGR_F_GetFieldAsString(Ogr_feature, i);
		    if (Ogr_fstring && *Ogr_fstring) {
			if (Ogr_ftype == OFTInteger ||
#if GDAL_VERSION_NUM >= 2000000
                            Ogr_ftype == OFTInteger64 ||
#endif
                            Ogr_ftype == OFTReal) {
			    sprintf(buf, ", %s", Ogr_fstring);
			}
#if GDAL_VERSION_NUM >= 1320
			    /* should we use OGR_F_GetFieldAsDateTime() here ? */
			else if (Ogr_ftype == OFTDate || Ogr_ftype == OFTTime
				 || Ogr_ftype == OFTDateTime) {
			    char *newbuf;

			    db_set_string(&strval, (char *)Ogr_fstring);
			    db_double_quote_string(&strval);
			    sprintf(buf, ", '%s'", db_get_string(&strval));
			    newbuf = G_str_replace(buf, "/", "-");	/* fix 2001/10/21 to 2001-10-21 */
			    sprintf(buf, "%s", newbuf);
			}
#endif
			else if (Ogr_ftype == OFTString ||
			         Ogr_ftype == OFTStringList ||
				 Ogr_ftype == OFTIntegerList 
#if GDAL_VERSION_NUM >= 2000000
                                 || Ogr_ftype == OFTInteger64List
#endif
                                 ) {
			    db_set_string(&strval, (char *)Ogr_fstring);
			    db_double_quote_string(&strval);
			    sprintf(buf, ", '%s'", db_get_string(&strval));
			}
			else {
			    /* column type not supported */
			    buf[0] = 0;
			}
		    }
		    else {
			/* G_warning (_("Column value not set" )); */
			if (Ogr_ftype == OFTInteger ||
#if GDAL_VERSION_NUM >= 2000000
                            Ogr_ftype == OFTInteger64 ||
#endif
                            Ogr_ftype == OFTReal) {
			    sprintf(buf, ", NULL");
			}
#if GDAL_VERSION_NUM >= 1320
			else if (Ogr_ftype == OFTDate ||
				 Ogr_ftype == OFTTime || 
				 Ogr_ftype == OFTDateTime) {
			    sprintf(buf, ", NULL");
			}
#endif
			else if (Ogr_ftype == OFTString ||
			         Ogr_ftype == OFTStringList ||
				 Ogr_ftype == OFTIntegerList
#if GDAL_VERSION_NUM >= 2000000
                                 || Ogr_ftype == OFTInteger64List
#endif
                                 ) {
			    sprintf(buf, ", NULL");
			}
			else {
			    /* column type not supported */
			    buf[0] = 0;
			}
		    }
		    db_append_string(&sql, buf);
		}
		db_append_string(&sql, " )");
		G_debug(3, "%s", db_get_string(&sql));

		if (db_execute_immediate(driver, &sql) != DB_OK) {
		    db_close_database(driver);
		    db_shutdown_driver(driver);
		    G_fatal_error(_("Cannot insert new row for input layer <%s>: %s"),
				  layer_names[layer], db_get_string(&sql));
		}
	    }

	    OGR_F_Destroy(Ogr_feature);
	    cat++;
	}
	G_percent(1, 1, 1);	/* finish it */

	if (!flag.notab->answer) {
	    db_commit_transaction(driver);
	    db_close_database_shutdown_driver(driver);
	}

	if (nogeom > 0)
	    G_warning(_("%d %s without geometry in input layer <%s> skipped"),
	              nogeom, nogeom == 1 ? _("feature") : _("features"),
		      layer_names[layer]);
    }

    delete_table = Vect_maptype(&Map) != GV_FORMAT_NATIVE;

    /* create index - must fail on non-unique categories */
    if (!flag.notab->answer) {
	for (layer = 0; layer < nlayers; layer++) {
	    Fi = Vect_get_field(&Map, layer + 1);
	    driver =
		db_start_driver_open_database(Fi->driver,
					      Vect_subst_var(Fi->database,
							     &Map));

	    if (!delete_table) {
		if (db_create_index2(driver, Fi->table, Fi->key) != DB_OK)
		    G_fatal_error(_("Unable to create index for table <%s>, key <%s>"),
			      Fi->table, Fi->key);
	    }
	    else {
		sprintf(buf, "drop table %s", Fi->table);
		db_set_string(&sql, buf);
		if (db_execute_immediate(driver, &sql) != DB_OK) {
		    G_fatal_error(_("Unable to drop table: '%s'"),
				  db_get_string(&sql));
		}
	    }
	    db_close_database_shutdown_driver(driver);
	}
    }
    /* attribute tables are now done */

    separator = "-----------------------------------------------------";
    G_message("%s", separator);

    if (use_tmp_vect) {
	/* TODO: is it necessary to build here? probably not, consumes time */
	/* GV_BUILD_BASE is sufficient to toggle boundary cleaning */
	Vect_build_partial(&Tmp, GV_BUILD_BASE);
    }

    /* make this a separate function ?
     * no, too many arguments */
    if (use_tmp_vect && !flag.no_clean->answer &&
	Vect_get_num_primitives(Out, GV_BOUNDARY) > 0) {
	int ret, centr, otype, n_nocat;
	CENTR *Centr;
	struct spatial_index si;
	double x, y, total_area, overlap_area, nocat_area;
	struct line_pnts *Points;
	int nmodif;

	Points = Vect_new_line_struct();

	G_message("%s", separator);

	/* the principal purpose is to convert non-topological polygons to 
	 * topological areas */
	G_message(_("Cleaning polygons"));

	if (snap >= 0) {
	    G_message("%s", separator);
	    G_message(_("Snapping boundaries (threshold = %.3e)..."), snap);
	    Vect_snap_lines(&Tmp, GV_BOUNDARY, snap, NULL);
	}

	/* It is not to clean to snap centroids, but I have seen data with 2 duplicate polygons
	 * (as far as decimal places were printed) and centroids were not identical */
	/* Disabled, because the mechanism has changed:
	 * at this stage, there are no centroids yet, centroids are caluclated 
	 * later for output areas, not fo input polygons */
	/*
	   fprintf ( stderr, separator );
	   fprintf ( stderr, "Snap centroids (threshold 0.000001):\n" );
	   Vect_snap_lines ( &Map, GV_CENTROID, 0.000001, NULL, stderr );
	 */

	G_message("%s", separator);
	G_message(_("Breaking polygons..."));
	Vect_break_polygons(&Tmp, GV_BOUNDARY, NULL);

	/* It is important to remove also duplicate centroids in case of duplicate input polygons */
	G_message("%s", separator);
	G_message(_("Removing duplicates..."));
	Vect_remove_duplicates(&Tmp, GV_BOUNDARY | GV_CENTROID, NULL);

	/* in non-pathological cases, the bulk of the cleaning is now done */

	/* Vect_clean_small_angles_at_nodes() can change the geometry so that new intersections
	 * are created. We must call Vect_break_lines(), Vect_remove_duplicates()
	 * and Vect_clean_small_angles_at_nodes() until no more small angles are found */
	do {
	    G_message("%s", separator);
	    G_message(_("Breaking boundaries..."));
	    Vect_break_lines(&Tmp, GV_BOUNDARY, NULL);

	    G_message("%s", separator);
	    G_message(_("Removing duplicates..."));
	    Vect_remove_duplicates(&Tmp, GV_BOUNDARY, NULL);

	    G_message("%s", separator);
	    G_message(_("Cleaning boundaries at nodes..."));
	    nmodif =
		Vect_clean_small_angles_at_nodes(&Tmp, GV_BOUNDARY, NULL);
	} while (nmodif > 0);

	/* merge boundaries */
	G_message("%s", separator);
	G_message(_("Merging boundaries..."));
	Vect_merge_lines(&Tmp, GV_BOUNDARY, NULL, NULL);

	G_message("%s", separator);
	if (type & GV_BOUNDARY) {	/* that means lines were converted to boundaries */
	    G_message(_("Changing boundary dangles to lines..."));
	    Vect_chtype_dangles(&Tmp, -1.0, NULL);
	}
	else {
	    G_message(_("Removing dangles..."));
	    Vect_remove_dangles(&Tmp, GV_BOUNDARY, -1.0, NULL);
	}

	G_message("%s", separator);
	Vect_build_partial(&Tmp, GV_BUILD_AREAS);

	G_message("%s", separator);
	if (type & GV_BOUNDARY) {
	    G_message(_("Changing boundary bridges to lines..."));
	    Vect_chtype_bridges(&Tmp, NULL, &nmodif, NULL);
	    if (nmodif)
		Vect_build_partial(&Tmp, GV_BUILD_NONE);
	}
	else {
	    G_message(_("Removing bridges..."));
	    Vect_remove_bridges(&Tmp, NULL, &nmodif, NULL);
	    if (nmodif)
		Vect_build_partial(&Tmp, GV_BUILD_NONE);
	}

	/* Boundaries are hopefully clean, build areas */
	G_message("%s", separator);
	Vect_build_partial(&Tmp, GV_BUILD_NONE);
	Vect_build_partial(&Tmp, GV_BUILD_ATTACH_ISLES);

	/* Calculate new centroids for all areas, centroids have the same id as area */
	ncentr = Vect_get_num_areas(&Tmp);
	G_debug(3, "%d centroids/areas", ncentr);

	Centr = (CENTR *) G_calloc(ncentr + 1, sizeof(CENTR));
	Vect_spatial_index_init(&si, 0);
	for (centr = 1; centr <= ncentr; centr++) {
	    Centr[centr].valid = 0;
	    Centr[centr].cats = Vect_new_cats_struct();
	    ret = Vect_get_point_in_area(&Tmp, centr, &x, &y);
	    if (ret < 0) {
		G_warning(_("Unable to calculate area centroid"));
		continue;
	    }

	    Centr[centr].x = x;
	    Centr[centr].y = y;
	    Centr[centr].valid = 1;
	    box.N = box.S = y;
	    box.E = box.W = x;
	    box.T = box.B = 0;
	    Vect_spatial_index_add_item(&si, centr, &box);
	}

	/* Go through all layers and find centroids for each polygon */
	OGR_iterator_reset(&OGR_iter);
	for (layer = 0; layer < nlayers; layer++) {
	    G_message("%s", separator);
	    G_message(_("Finding centroids for OGR layer <%s>..."), layer_names[layer]);
	    layer_id = layers[layer];
	    Ogr_layer = ds_getlayerbyindex(Ogr_ds, layer_id);
	    Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);

	    igeom = -1;
#if GDAL_VERSION_NUM >= 1110000
	    if (param.geom->answer)
		igeom = OGR_FD_GetGeomFieldIndex(Ogr_featuredefn, param.geom->answer);
#endif

	    cat = 0;		/* field = layer + 1 */
	    while ((Ogr_feature = ogr_getnextfeature(&OGR_iter, layer_id,
						     layer_names[layer],
						     poSpatialFilter[layer],
						     attr_filter)) != NULL) {
		G_percent(cat, n_features[layer], 2);

		/* Category */
		if (key_idx[layer] > -1)
		    cat = OGR_F_GetFieldAsInteger(Ogr_feature, key_idx[layer]);
		else
		    cat++;

		/* Geometry */
#if GDAL_VERSION_NUM >= 1110000
		Ogr_featuredefn = OGR_iter.Ogr_featuredefn;
		for (i = 0; i < OGR_FD_GetGeomFieldCount(Ogr_featuredefn); i++) {
		    if (igeom > -1 && i != igeom)
			continue; /* use only geometry defined via param.geom */
	    
		    Ogr_geometry = OGR_F_GetGeomFieldRef(Ogr_feature, i);
#else
		    Ogr_geometry = OGR_F_GetGeometryRef(Ogr_feature);
#endif
		    if (Ogr_geometry != NULL) {
#if GDAL_VERSION_NUM >= 2000000
			Ogr_geometry = OGR_G_GetLinearGeometry(Ogr_geometry, 0, NULL);
		    }
		    if (Ogr_geometry != NULL) {
#endif
			centroid(Ogr_geometry, Centr, &si, layer + 1, cat,
				 min_area, type);
#if GDAL_VERSION_NUM >= 2000000
			OGR_G_DestroyGeometry(Ogr_geometry);
#endif
		    }
#if GDAL_VERSION_NUM >= 1110000
		}
#endif
		OGR_F_Destroy(Ogr_feature);
	    }
	    G_percent(1, 1, 1);
	}

	/* Write centroids */
	G_message("%s", separator);
	G_message(_("Writing centroids..."));

	n_overlaps = n_nocat = 0;
	total_area = overlap_area = nocat_area = 0.0;
	for (centr = 1; centr <= ncentr; centr++) {
	    double area;
	    
	    G_percent(centr, ncentr, 2);

	    area = Vect_get_area_area(&Tmp, centr);
	    total_area += area;

	    if (!(Centr[centr].valid)) {
		continue;
	    }

	    if (Centr[centr].cats->n_cats == 0) {
		nocat_area += area;
		n_nocat++;
		continue;
	    }

	    if (Centr[centr].cats->n_cats > 1) {
		Vect_cat_set(Centr[centr].cats, nlayers + 1,
			     Centr[centr].cats->n_cats);
		overlap_area += area;
		n_overlaps++;
	    }

	    Vect_reset_line(Points);
	    Vect_append_point(Points, Centr[centr].x, Centr[centr].y, 0.0);
	    if (type & GV_POINT)
		otype = GV_POINT;
	    else
		otype = GV_CENTROID;
	    Vect_write_line(&Tmp, otype, Points, Centr[centr].cats);
	}
	if (Centr)
	    G_free(Centr);
	    
	Vect_spatial_index_destroy(&si);

	if (n_overlaps > 0) {
	    G_warning(_("%d areas represent more (overlapping) features, because polygons overlap "
		       "in input layer(s). Such areas are linked to more than 1 row in attribute table. "
		       "The number of features for those areas is stored as category in layer %d"),
		      n_overlaps, nlayers + 1);
	}

	G_message("%s", separator);

	Vect_hist_write(&Map, separator);
	Vect_hist_write(&Map, "\n");
	sprintf(buf, _("%d input polygons\n"), n_polygons);
	G_message(_("%d input polygons"), n_polygons);
	Vect_hist_write(&Map, buf);

	sprintf(buf, _("Total area: %G (%d areas)\n"), total_area, ncentr);
	G_message(_("Total area: %G (%d areas)"), total_area, ncentr);
	Vect_hist_write(&Map, buf);

	sprintf(buf, _("Overlapping area: %G (%d areas)\n"), overlap_area,
		n_overlaps);
	if (n_overlaps) {
	    G_message(_("Overlapping area: %G (%d areas)"), overlap_area,
		      n_overlaps);
	}
	Vect_hist_write(&Map, buf);

	sprintf(buf, _("Area without category: %G (%d areas)\n"), nocat_area,
		n_nocat);
	if (n_nocat) {
	    G_message(_("Area without category: %G (%d areas)"), nocat_area,
		      n_nocat);
	}
	Vect_hist_write(&Map, buf);
	G_message("%s", separator);
    }

    ds_close(Ogr_ds);

    if (use_tmp_vect) {
	/* Copy temporary vector to output vector */
	Vect_copy_map_lines(&Tmp, &Map);
	/* release memory occupied by topo, we may need that memory for main output */
	Vect_set_release_support(&Tmp);
	Vect_close(&Tmp); /* temporary map is deleted automatically */
    }

    Vect_build(&Map);
#if 0
    /* disabled, Vect_topo_check() is quite slow */
    if (flag.no_clean->answer)
	Vect_topo_check(&Map, NULL);
#endif

    if (n_polygons && nlayers == 1) {
	/* test for topological errors */
	/* this test is not perfect:
	 * small gaps (areas without centroid) are not detected
	 * small gaps may also be true gaps */
	ncentr = Vect_get_num_primitives(&Map, GV_CENTROID);
	if (ncentr != n_polygons || n_overlaps) {
	    double min_snap, max_snap;
	    int exp;

	    Vect_get_map_box(&Map, &box);
	    
	    if (abs(box.E) > abs(box.W))
		xmax = abs(box.E);
	    else
		xmax = abs(box.W);
	    if (abs(box.N) > abs(box.S))
		ymax = abs(box.N);
	    else
		ymax = abs(box.S);

	    if (xmax < ymax)
		xmax = ymax;

	    /* double precision ULP */
	    min_snap = frexp(xmax, &exp);
	    exp -= 52;
	    min_snap = ldexp(min_snap, exp);
	    /* human readable */
	    min_snap = log10(min_snap);
	    if (min_snap < 0)
		min_snap = (int)min_snap;
	    else
		min_snap = (int)min_snap + 1;
	    min_snap = pow(10, min_snap);

	    /* single precision ULP */
	    max_snap = frexp(xmax, &exp);
	    exp -= 23;
	    max_snap = ldexp(max_snap, exp);
	    /* human readable */
	    max_snap = log10(max_snap);
	    if (max_snap < 0)
		max_snap = (int)max_snap;
	    else
		max_snap = (int)max_snap + 1;
	    max_snap = pow(10, max_snap);

	    G_important_message("%s", separator);
	    if (n_overlaps) {
		G_important_message(_("Some input polygons are overlapping each other."));
		G_important_message(_("If overlapping is not desired, the data need to be cleaned."));

		if (snap < max_snap) {
		    G_important_message(_("The input could be cleaned by snapping vertices to each other."));
		    G_important_message(_("Estimated range of snapping threshold: [%g, %g]"), min_snap, max_snap);
		}

		if (snap < min_snap) {
		    G_important_message(_("Try to import again, snapping with at least %g: 'snap=%g'"), min_snap, min_snap);
		}
		else if (snap < max_snap) {
		    min_snap = snap * 10;
		    G_important_message(_("Try to import again, snapping with %g: 'snap=%g'"), min_snap, min_snap);
		}
		else
		    /* assume manual cleaning is required */
		    G_important_message(_("Manual cleaning may be needed."));
	    }
	    else {
		if (ncentr < n_polygons) {
		    G_important_message(_("%d input polygons got lost during import."), n_polygons - ncentr);
		}
		if (ncentr > n_polygons) {
		    G_important_message(_("%d additional areas where created during import."), ncentr - n_polygons);
		}
		if (snap > 0) {
		    G_important_message(_("The snapping threshold %g might be too large."), snap);
		    G_important_message(_("Estimated range of snapping threshold: [%g, %g]"), min_snap, max_snap);
		    /* assume manual cleaning is required */
		    G_important_message(_("Manual cleaning may be needed."));
		}
		else {
		    G_important_message(_("The input could be cleaned by snapping vertices to each other."));
		    G_important_message(_("Estimated range of snapping threshold: [%g, %g]"), min_snap, max_snap);
		}
	    }

	}
    }

    Vect_get_map_box(&Map, &box);
    if (0 != Vect_close(&Map))
        G_fatal_error(_("Import failed"));

    /* -------------------------------------------------------------------- */
    /*      Extend current window based on dataset.                         */
    /* -------------------------------------------------------------------- */
    if (flag.extend->answer) {
	if (strcmp(G_mapset(), "PERMANENT") == 0)
	    /* fixme: expand WIND and DEFAULT_WIND independently. (currently 
		WIND gets forgotten and DEFAULT_WIND is expanded for both) */
	    G_get_default_window(&cur_wind);
	else
	    G_get_window(&cur_wind);

	cur_wind.north = MAX(cur_wind.north, box.N);
	cur_wind.south = MIN(cur_wind.south, box.S);
	cur_wind.west = MIN(cur_wind.west, box.W);
	cur_wind.east = MAX(cur_wind.east, box.E);

	cur_wind.rows = (int)ceil((cur_wind.north - cur_wind.south)
				  / cur_wind.ns_res);
	cur_wind.south = cur_wind.north - cur_wind.rows * cur_wind.ns_res;

	cur_wind.cols = (int)ceil((cur_wind.east - cur_wind.west)
				  / cur_wind.ew_res);
	cur_wind.east = cur_wind.west + cur_wind.cols * cur_wind.ew_res;

	if (strcmp(G_mapset(), "PERMANENT") == 0) {
	    G_put_element_window(&cur_wind, "", "DEFAULT_WIND");
	    G_message(_("Default region for this location updated"));
	}
	G_put_window(&cur_wind);
	G_message(_("Region for the current mapset updated"));
    }

    if (input3d && flag.force2d->answer)
	G_warning(_("Input data contains 3D features. Created vector is 2D only, "
		   "disable -2 flag to import 3D vector."));

    exit(EXIT_SUCCESS);
}

void OGR_iterator_init(struct OGR_iterator *OGR_iter, ds_t *Ogr_ds,
                       char *dsn, int nlayers,
		       int ogr_interleaved_reading)
{
    OGR_iter->Ogr_ds = Ogr_ds;
    OGR_iter->dsn = dsn;
    OGR_iter->nlayers = nlayers;
    OGR_iter->ogr_interleaved_reading = ogr_interleaved_reading;
    OGR_iter->requested_layer = -1;
    OGR_iter->curr_layer = -1;
    OGR_iter->Ogr_layer = NULL;
    OGR_iter->has_nonempty_layers = 0;
    OGR_iter->done = 0;

    if (OGR_iter->ogr_interleaved_reading) {
#if GDAL_VERSION_NUM >= 2020000
	G_verbose_message(_("Using GDAL 2.2+ style interleaved reading for GDAL version %d"),
			  GDAL_VERSION_NUM); 
#else
	G_verbose_message(_("Using GDAL 1.x style interleaved reading for GDAL version %d"),
			  GDAL_VERSION_NUM); 
#endif
    }
}

void OGR_iterator_reset(struct OGR_iterator *OGR_iter)
{
#if GDAL_VERSION_NUM >= 2020000
    GDALDatasetResetReading(*(OGR_iter->Ogr_ds));
#endif
    OGR_iter->requested_layer = -1;
    OGR_iter->curr_layer = -1;
    OGR_iter->Ogr_layer = NULL;
    OGR_iter->has_nonempty_layers = 0;
    OGR_iter->done = 0;
}

OGRFeatureH ogr_getnextfeature(struct OGR_iterator *OGR_iter,
                               int layer, char *layer_name,
			       OGRGeometryH poSpatialFilter,
			       const char *attr_filter)
{
    if (OGR_iter->requested_layer != layer) {

	/* reset OGR reading */
	if (!OGR_iter->ogr_interleaved_reading) {
	    OGR_iter->curr_layer = layer;
	    OGR_iter->Ogr_layer = ds_getlayerbyindex(*(OGR_iter->Ogr_ds), OGR_iter->curr_layer);
	    OGR_iter->Ogr_featuredefn = OGR_L_GetLayerDefn(OGR_iter->Ogr_layer);
	    OGR_L_ResetReading(OGR_iter->Ogr_layer);
	}
	else {
	    int i;

	    /* clear filters */
	    for (i = 0; i < OGR_iter->nlayers; i++) {
		OGR_iter->Ogr_layer = ds_getlayerbyindex(*(OGR_iter->Ogr_ds), i);
		OGR_L_SetSpatialFilter(OGR_iter->Ogr_layer, NULL);
		OGR_L_SetAttributeFilter(OGR_iter->Ogr_layer, NULL);
	    }

#if GDAL_VERSION_NUM >= 2020000
	    GDALDatasetResetReading(*(OGR_iter->Ogr_ds));
#else
	    /* need to re-open OGR DSN in order to start reading from the beginning
	     * NOTE: any constraints are lost */
	    OGR_DS_Destroy(*(OGR_iter->Ogr_ds));
	    *(OGR_iter->Ogr_ds) = OGROpen(OGR_iter->dsn, FALSE, NULL);
	    if (*(OGR_iter->Ogr_ds) == NULL)
		G_fatal_error(_("Unable to re-open data source <%s>"), OGR_iter->dsn);
	    OGR_iter->Ogr_layer = OGR_DS_GetLayer(*(OGR_iter->Ogr_ds), layer);
	    OGR_iter->curr_layer = 0;
	    OGR_iter->has_nonempty_layers = 0;
#endif
	    OGR_iter->Ogr_layer = ds_getlayerbyindex(*(OGR_iter->Ogr_ds), layer);
	    OGR_iter->Ogr_featuredefn = OGR_L_GetLayerDefn(OGR_iter->Ogr_layer);
	    OGR_L_SetSpatialFilter(OGR_iter->Ogr_layer, poSpatialFilter);
	    if (OGR_L_SetAttributeFilter(OGR_iter->Ogr_layer, attr_filter) != OGRERR_NONE)
		G_fatal_error(_("Error setting attribute filter '%s'"),
		              attr_filter);
#if GDAL_VERSION_NUM < 2020000
	    OGR_iter->Ogr_layer = OGR_DS_GetLayer(*(OGR_iter->Ogr_ds), OGR_iter->curr_layer);
#endif
	}
	OGR_iter->requested_layer = layer;
	OGR_iter->done = 0;
    }

    if (OGR_iter->done == 1)
	return NULL;

    if (!OGR_iter->ogr_interleaved_reading) {
	OGRFeatureH Ogr_feature;

	Ogr_feature = OGR_L_GetNextFeature(OGR_iter->Ogr_layer);
	if (Ogr_feature == NULL) {
	    OGR_iter->Ogr_layer = NULL;
	    OGR_iter->done = 1;
	}

	return Ogr_feature;
    }
    else {
	OGRFeatureH Ogr_feature = NULL;

	/* fetch next feature */
#if GDAL_VERSION_NUM >= 2020000
	while (1) {
	    OGR_iter->Ogr_layer = NULL;
	    Ogr_feature = GDALDatasetGetNextFeature(*(OGR_iter->Ogr_ds),
							&(OGR_iter->Ogr_layer),
							NULL, NULL, NULL);

	    if (Ogr_feature == NULL) {
		OGR_iter->Ogr_layer = NULL;
		OGR_iter->done = 1;

		return Ogr_feature;
	    }
	    if (OGR_iter->Ogr_layer != NULL) {
		const char *ln = OGR_L_GetName(OGR_iter->Ogr_layer);
		
		if (ln && *ln && strcmp(ln, layer_name) == 0) {

		    return Ogr_feature;
		}
	    }
	    OGR_F_Destroy(Ogr_feature);
	    OGR_iter->Ogr_layer = NULL;
	}
#else
	while (1) {
	    Ogr_feature = OGR_L_GetNextFeature(OGR_iter->Ogr_layer);
	    if (Ogr_feature != NULL) {
		OGR_iter->has_nonempty_layers = 1;
		if (OGR_iter->curr_layer != layer)
		    OGR_F_Destroy(Ogr_feature);
		else
		    return Ogr_feature;
	    }
	    else {
		OGR_iter->curr_layer++;
		if (OGR_iter->curr_layer == OGR_iter->nlayers) {
		    if (!OGR_iter->has_nonempty_layers) {
			OGR_iter->Ogr_layer = NULL;
			OGR_iter->done = 1;

			return NULL;
		    }
		    else {
			OGR_iter->curr_layer = 0;
			OGR_iter->has_nonempty_layers = 0;
		    }
		}
		G_debug(3, "advancing to layer %d ...", OGR_iter->curr_layer);
		OGR_iter->Ogr_layer = OGR_DS_GetLayer(*(OGR_iter->Ogr_ds), OGR_iter->curr_layer);
		OGR_iter->Ogr_featuredefn = OGR_L_GetLayerDefn(OGR_iter->Ogr_layer);
	    }
	}
#endif
    }

    return NULL;
}

/* get projection info of OGR layer in GRASS format
 * return 0 on success (some non-xy SRS)
 * return 1 if no SRS available
 * return 2 if SRS available but unreadable */
int get_layer_proj(OGRLayerH Ogr_layer, struct Cell_head *cellhd,
		   struct Key_Value **proj_info, struct Key_Value **proj_units,
		   char *geom_col, int verbose)
{
    OGRSpatialReferenceH Ogr_projection;

    Ogr_projection = NULL;
    *proj_info = NULL;
    *proj_units = NULL;
    G_get_window(cellhd);

    /* Fetch input layer projection in GRASS form. */
#if GDAL_VERSION_NUM >= 1110000
    if (geom_col) {
	int igeom;
        OGRGeomFieldDefnH Ogr_geomdefn;
	OGRFeatureDefnH Ogr_featuredefn;
        
        Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);
        igeom = OGR_FD_GetGeomFieldIndex(Ogr_featuredefn, geom_col);
        if (igeom < 0)
            G_fatal_error(_("Geometry column <%s> not found in input layer <%s>"),
                          geom_col, OGR_L_GetName(Ogr_layer));
        Ogr_geomdefn = OGR_FD_GetGeomFieldDefn(Ogr_featuredefn, igeom);
        Ogr_projection = OGR_GFld_GetSpatialRef(Ogr_geomdefn);
    }
    else {
        Ogr_projection = OGR_L_GetSpatialRef(Ogr_layer);
    }
#else
    Ogr_projection = OGR_L_GetSpatialRef(Ogr_layer);	/* should not be freed later */
#endif

    /* verbose is used only when comparing input SRS to GRASS projection,
     * not when comparing SRS's of several input layers */
    if (GPJ_osr_to_grass(cellhd, proj_info,
			 proj_units, Ogr_projection, 0) < 0) {
	/* TODO: GPJ_osr_to_grass() does not return anything < 0
	 * check with GRASS 6 and GRASS 5 */
	G_warning(_("Unable to convert input layer projection information to "
		   "GRASS format for checking"));
	if (verbose && Ogr_projection != NULL) {
	    char *wkt = NULL;

	    if (OSRExportToPrettyWkt(Ogr_projection, &wkt, FALSE) != OGRERR_NONE) {
		G_warning(_("Can't get WKT-style parameter string"));
	    }
	    else if (wkt) {
		G_important_message(_("WKT-style definition:\n%s"), wkt);
	    }
	}

	return 2;
    }
    /* custom checks because if in doubt GPJ_osr_to_grass() returns a 
     * xy CRS */
    if (Ogr_projection == NULL) {
	if (verbose) {
	    G_important_message(_("No OGR projection available for layer <%s>"),
				OGR_L_GetName(Ogr_layer));
	}

	return 1;
    }

    if (!OSRIsProjected(Ogr_projection) && !OSRIsGeographic(Ogr_projection)) {
	G_important_message(_("OGR projection for layer <%s> does not contain a valid SRS"),
			    OGR_L_GetName(Ogr_layer));

	if (verbose) {
	    char *wkt = NULL;

	    if (OSRExportToPrettyWkt(Ogr_projection, &wkt, FALSE) != OGRERR_NONE) {
		G_important_message(_("Can't get WKT-style parameter string"));
	    }
	    else if (wkt) {
		G_important_message(_("WKT-style definition:\n%s"), wkt);
	    }
	}

	return 2;
    }

    char *pszProj4 = NULL;

    if (OSRExportToProj4(Ogr_projection, &pszProj4) != OGRERR_NONE) {
	G_important_message(_("OGR projection for layer <%s> can not be converted to proj4"),
			    OGR_L_GetName(Ogr_layer));

	if (verbose) {
	    char *wkt = NULL;

	    if (OSRExportToPrettyWkt(Ogr_projection, &wkt, FALSE) != OGRERR_NONE) {
		G_important_message(_("Can't get WKT-style parameter string"));
	    }
	    else if (wkt) {
		G_important_message(_("WKT-style definition:\n%s"), wkt);
	    }
	}

	return 2;
    }

    return 0;
}

/* compare projections of all OGR layers
 * return 0 if all layers have the same projection
 * return 1 if layer projections differ */
int cmp_layer_srs(ds_t Ogr_ds, int nlayers, int *layers,
		  char **layer_names, char *geom_col)
{
    int layer;
    struct Key_Value *proj_info1, *proj_units1;
    struct Key_Value *proj_info2, *proj_units2;
    struct Cell_head cellhd1, cellhd2;
    OGRLayerH Ogr_layer;

    if (nlayers == 1)
	return 0;

    proj_info1 = proj_units1 = NULL;
    proj_info2 = proj_units2 = NULL;

    layer = 0;
    do {
	/* Get first SRS */
	Ogr_layer = ds_getlayerbyindex(Ogr_ds, layers[layer]);

	if (get_layer_proj(Ogr_layer, &cellhd1, &proj_info1, &proj_units1,
			   geom_col, 0) == 0) {
	    break;
	}
	layer++;
    } while (layer < nlayers);

    if (layer == nlayers) {
	/* could not get layer proj in GRASS format for any of the layers
	 * -> projections of all layers are the same, i.e. unreadable by GRASS */
	G_warning(_("Layer projections are unreadable"));
	if (proj_info1)
	    G_free_key_value(proj_info1);
	if (proj_units1)
	    G_free_key_value(proj_units1);

	return 0;
    }
    if (layer > 0) {
	/* could not get layer proj in GRASS format for at least one of the layers
	 * -> mix of unreadable and readable projections  */
	G_warning(_("Projection for layer <%s> is unreadable"),
	          layer_names[layer]);
	if (proj_info1)
	    G_free_key_value(proj_info1);
	if (proj_units1)
	    G_free_key_value(proj_units1);

	return 1;
    }

    for (layer = 1; layer < nlayers; layer++) {
	/* Get SRS of other layer(s) */
	Ogr_layer = ds_getlayerbyindex(Ogr_ds, layers[layer]);
	if (get_layer_proj(Ogr_layer, &cellhd2, &proj_info2, &proj_units2,
			   geom_col, 0) != 0) {
	    G_free_key_value(proj_info1);
	    G_free_key_value(proj_units1);

	    return 1;
	}

	if (cellhd1.proj != cellhd2.proj
	    || G_compare_projections(proj_info1, proj_units1,
				     proj_info2, proj_units2) != TRUE) {
	    if (proj_info1)
		G_free_key_value(proj_info1);
	    if (proj_units1)
		G_free_key_value(proj_units1);
	    if (proj_info2)
		G_free_key_value(proj_info2);
	    if (proj_units2)
		G_free_key_value(proj_units2);
	    
	    G_warning(_("Projection of layer <%s> is different from "
			"projection of layer <%s>"),
			layer_names[layer], layer_names[layer - 1]);

	    return 1;
	 }
	if (proj_info2)
	    G_free_key_value(proj_info2);
	if (proj_units2)
	    G_free_key_value(proj_units2);
    }
    if (proj_info1)
	G_free_key_value(proj_info1);
    if (proj_units1)
	G_free_key_value(proj_units1);

    return 0;
}

int create_spatial_filter(ds_t Ogr_ds, OGRGeometryH *poSpatialFilter,
                          int nlayers, int *layers, char **layer_names,
                          double *xmin, double *ymin,
			  double *xmax, double *ymax,
			  int use_region, struct Option *spat)
{
    int layer;
    int have_spatial_filter;
    int *have_ogr_extent;
    double *xminl, *yminl, *xmaxl, *ymaxl;
    OGRLayerH Ogr_layer;
    OGREnvelope oExt;
    OGRGeometryH Ogr_oRing;
    struct Cell_head cur_wind;

    /* fetch extents */
    have_ogr_extent = (int *)G_malloc(nlayers * sizeof(int));
    xminl = (double *)G_malloc(nlayers * sizeof(double));
    xmaxl = (double *)G_malloc(nlayers * sizeof(double));
    yminl = (double *)G_malloc(nlayers * sizeof(double));
    ymaxl = (double *)G_malloc(nlayers * sizeof(double));

    for (layer = 0; layer < nlayers; layer++) {
	Ogr_layer = ds_getlayerbyindex(Ogr_ds, layers[layer]);
	have_ogr_extent[layer] = 0;
	if ((OGR_L_GetExtent(Ogr_layer, &oExt, 1)) == OGRERR_NONE) {
	    xminl[layer] = oExt.MinX;
	    xmaxl[layer] = oExt.MaxX;
	    yminl[layer] = oExt.MinY;
	    ymaxl[layer] = oExt.MaxY;

	    /* use OGR extents if possible, needed to skip corrupted data
	     * in OGR dsn/layer */
	    have_ogr_extent[layer] = 1;
	}
	/* OGR_L_GetExtent(): 
	 * Note that some implementations of this method may alter 
	 * the read cursor of the layer. */
#if GDAL_VERSION_NUM >= 2020000
	GDALDatasetResetReading(Ogr_ds);
#else
	OGR_L_ResetReading(Ogr_layer);
#endif
    }

    /* set spatial filter */
    if (use_region && spat->answer)
	G_fatal_error(_("Select either the current region flag or the spatial option, not both"));
    if (use_region) {
	G_get_window(&cur_wind);
	*xmin = cur_wind.west;
	*xmax = cur_wind.east;
	*ymin = cur_wind.south;
	*ymax = cur_wind.north;
    }
    if (spat->answer) {
	int i;
	/* See as reference: gdal/ogr/ogr_capi_test.c */

	/* cut out a piece of the map */
	/* order: xmin,ymin,xmax,ymax */
	i = 0;
	while (spat->answers[i]) {
	    if (i == 0)
		*xmin = atof(spat->answers[i]);
	    if (i == 1)
		*ymin = atof(spat->answers[i]);
	    if (i == 2)
		*xmax = atof(spat->answers[i]);
	    if (i == 3)
		*ymax = atof(spat->answers[i]);
	    i++;
	}
	if (i != 4)
	    G_fatal_error(_("4 parameters required for 'spatial' parameter"));
	if (*xmin > *xmax)
	    G_fatal_error(_("xmin is larger than xmax in 'spatial' parameters"));
	if (*ymin > *ymax)
	    G_fatal_error(_("ymin is larger than ymax in 'spatial' parameters"));
    }
    if (use_region || spat->answer) {
	G_debug(2, "cut out with boundaries: xmin:%f ymin:%f xmax:%f ymax:%f",
		*xmin, *ymin, *xmax, *ymax);
    }

    /* create spatial filter for each layer */
    have_spatial_filter = 0;
    for (layer = 0; layer < nlayers; layer++) {
	int have_filter = 0;

	if (have_ogr_extent[layer]) {
	    if (*xmin <= *xmax && *ymin <= *ymax) {
		/* check for any overlap */
		if (xminl[layer] > *xmax || xmaxl[layer] < *xmin ||
		    yminl[layer] > *ymax || ymaxl[layer] < *ymin) {
		    G_warning(_("The spatial filter does not overlap with OGR layer <%s>. Nothing to import."),
			      layer_names[layer]);

		    xminl[layer] = *xmin;
		    xmaxl[layer] = *xmax;
		    yminl[layer] = *ymin;
		    ymaxl[layer] = *ymax;
		}
		else {
		    /* shrink with user options */
		    xminl[layer] = MAX(xminl[layer], *xmin);
		    xmaxl[layer] = MIN(xmaxl[layer], *xmax);
		    yminl[layer] = MAX(yminl[layer], *ymin);
		    ymaxl[layer] = MIN(ymaxl[layer], *ymax);
		}
	    }
	    have_filter = 1;
	}
	else if (*xmin <= *xmax && *ymin <= *ymax) {
	    xminl[layer] = *xmin;
	    xmaxl[layer] = *xmax;
	    yminl[layer] = *ymin;
	    ymaxl[layer] = *ymax;

	    have_filter = 1;
	}

	if (have_filter) {
	    /* some invalid features can be filtered out by using
	     * the layer's extents as spatial filter
	     * hopefully these filtered features are all invalid */
	    /* TODO/BUG:
	     * for OSM, a spatial filter applied on the 'points' layer 
	     * will also affect other layers */

	    /* in theory this could be an irregular polygon */
	    G_debug(2, "spatial filter for layer <%s>: xmin:%f ymin:%f xmax:%f ymax:%f",
		    layer_names[layer],
		    xminl[layer], yminl[layer],
		    xmaxl[layer], ymaxl[layer]);

	    poSpatialFilter[layer] = OGR_G_CreateGeometry(wkbPolygon);
	    Ogr_oRing = OGR_G_CreateGeometry(wkbLinearRing);
	    OGR_G_AddPoint_2D(Ogr_oRing, xminl[layer], yminl[layer]);
	    OGR_G_AddPoint_2D(Ogr_oRing, xminl[layer], ymaxl[layer]);
	    OGR_G_AddPoint_2D(Ogr_oRing, xmaxl[layer], ymaxl[layer]);
	    OGR_G_AddPoint_2D(Ogr_oRing, xmaxl[layer], yminl[layer]);
	    OGR_G_AddPoint_2D(Ogr_oRing, xminl[layer], yminl[layer]);
	    OGR_G_AddGeometryDirectly(poSpatialFilter[layer], Ogr_oRing);

	    have_spatial_filter = 1;
	}
	else
	    poSpatialFilter[layer] = NULL;
    }
    /* update xmin, xmax, ymin, ymax if possible */
    for (layer = 0; layer < nlayers; layer++) {
	if (have_ogr_extent[layer]) {
	    if (xmin > xmax) {
		*xmin = xminl[layer];
		*xmax = xmaxl[layer];
		*ymin = yminl[layer];
		*ymax = ymaxl[layer];
	    }
	    else {
		/* expand */
		*xmin = MIN(xminl[layer], *xmin);
		*xmax = MAX(xmaxl[layer], *xmax);
		*ymin = MIN(yminl[layer], *ymin);
		*ymax = MAX(ymaxl[layer], *ymax);
	    }
	}
    }
    G_free(have_ogr_extent);
    G_free(xminl);
    G_free(xmaxl);
    G_free(yminl);
    G_free(ymaxl);

    return have_spatial_filter;
}
