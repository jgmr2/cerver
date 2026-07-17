#ifndef DBFILLER_CSV_DICT_H
#define DBFILLER_CSV_DICT_H

/* Realistic-looking dummy data sourced from the sample CSVs vendored under
   data/ (Datablist's sample datasets), embedded at compile time in the
   headers under src/data/. All accessors lazily parse the datasets on
   first use. */

const char *csv_dict_random_first_name(void);
const char *csv_dict_random_last_name(void);
const char *csv_dict_random_company(void);
const char *csv_dict_random_city(void);
const char *csv_dict_random_country(void);
const char *csv_dict_random_email_domain(void);
const char *csv_dict_random_job_title(void);
const char *csv_dict_random_product_name(void);
const char *csv_dict_random_category(void);
const char *csv_dict_random_color(void);
const char *csv_dict_random_word(void); /* generic filler for untyped text columns */

#endif
