BEGIN;

CREATE TABLE all_users (
	id SERIAL PRIMARY KEY,
	name TEXT,
	passwd TEXT,
	alive BOOLEAN DEFAULT TRUE,
	money BIGINT DEFAULT 0,
	rank REAL DEFAULT 0,
	paid_posts INTEGER DEFAULT 0,
);

CREATE VIEW users AS
	SELECT * FROM all_users WHERE alive = TRUE;

CREATE UNIQUE INDEX user_name_idx ON all_users (lower(name)) WHERE alive = TRUE;

CREATE TABLE shopping_categories (
	id SERIAL PRIMARY KEY,
	name TEXT
);

CREATE TABLE shopping_items (
	id SERIAL PRIMARY KEY,
	category INTEGER REFERENCES shopping_categories,
	name TEXT,
	price INTEGER
);

CREATE TABLE shopping_records (
	id SERIAL PRIMARY KEY,
	user_id REFERENCES all_users,
	item INTEGER REFERENCES shopping_items,
	price TEXT,
	order_time TIMESTAMPTZ
);

COMMIT;
