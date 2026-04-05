-- Indices para consultas hot de Sakila.
CREATE INDEX IF NOT EXISTS idx_film_top_length_title_cover
ON film (length DESC, title ASC)
INCLUDE (release_year, rating)
WHERE length IS NOT NULL;

ANALYZE film;
