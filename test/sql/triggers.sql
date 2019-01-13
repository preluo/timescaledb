-- This file and its contents are licensed under the Apache License 2.0.
-- Please see the included NOTICE for copyright information and
-- LICENSE-APACHE for a copy of the license.

CREATE TABLE hyper (
  time BIGINT NOT NULL,
  device_id TEXT NOT NULL,
  sensor_1 NUMERIC NULL DEFAULT 1
);

CREATE OR REPLACE FUNCTION test_trigger()
    RETURNS TRIGGER LANGUAGE PLPGSQL AS
$BODY$
DECLARE
    cnt INTEGER;
BEGIN
    SELECT count(*) INTO cnt FROM hyper;
    RAISE WARNING 'FIRING trigger when: % level: % op: % cnt: % trigger_name %',
        tg_when, tg_level, tg_op, cnt, tg_name;

    IF TG_OP = 'DELETE' THEN
        RETURN OLD;
    END IF;
    RETURN NEW;
END
$BODY$;

-- row triggers: BEFORE
CREATE TRIGGER _0_test_trigger_insert
    BEFORE INSERT ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update
    BEFORE UPDATE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete
    BEFORE delete ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER z_test_trigger_all
    BEFORE INSERT OR UPDATE OR DELETE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

-- row triggers: AFTER
CREATE TRIGGER _0_test_trigger_insert_after
    AFTER INSERT ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_insert_after_when_dev1
    AFTER INSERT ON hyper
    FOR EACH ROW
    WHEN (NEW.device_id = 'dev1')
    EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update_after
    AFTER UPDATE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete_after
    AFTER delete ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER z_test_trigger_all_after
    AFTER INSERT OR UPDATE OR DELETE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

-- statement triggers: BEFORE
CREATE TRIGGER _0_test_trigger_insert_s_before
    BEFORE INSERT ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update_s_before
    BEFORE UPDATE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete_s_before
    BEFORE DELETE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

-- statement triggers: AFTER
CREATE TRIGGER _0_test_trigger_insert_s_after
    AFTER INSERT ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update_s_after
    AFTER UPDATE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete_s_after
    AFTER DELETE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();


SELECT * FROM create_hypertable('hyper', 'time', chunk_time_interval => 10);

--test triggers before create_hypertable
INSERT INTO hyper(time, device_id,sensor_1) VALUES
(1257987600000000000, 'dev1', 1);

INSERT INTO hyper(time, device_id,sensor_1) VALUES
(1257987700000000000, 'dev2', 1), (1257987800000000000, 'dev2', 1);

UPDATE hyper SET sensor_1 = 2;

DELETE FROM hyper;

--test drop trigger
DROP TRIGGER _0_test_trigger_insert ON hyper;
DROP TRIGGER _0_test_trigger_insert_s_before ON hyper;
DROP TRIGGER _0_test_trigger_insert_after ON hyper;
DROP TRIGGER _0_test_trigger_insert_s_after ON hyper;
INSERT INTO hyper(time, device_id,sensor_1) VALUES
(1257987600000000000, 'dev1', 1);
INSERT INTO hyper(time, device_id,sensor_1) VALUES
(1257987700000000000, 'dev2', 1), (1257987800000000000, 'dev2', 1);

DROP TRIGGER _0_test_trigger_update ON hyper;
DROP TRIGGER _0_test_trigger_update_s_before ON hyper;
DROP TRIGGER _0_test_trigger_update_after ON hyper;
DROP TRIGGER _0_test_trigger_update_s_after ON hyper;
UPDATE hyper SET sensor_1 = 2;

DROP TRIGGER _0_test_trigger_delete ON hyper;
DROP TRIGGER _0_test_trigger_delete_s_before ON hyper;
DROP TRIGGER _0_test_trigger_delete_after ON hyper;
DROP TRIGGER _0_test_trigger_delete_s_after ON hyper;
DELETE FROM hyper;

DROP TRIGGER z_test_trigger_all ON hyper;
DROP TRIGGER z_test_trigger_all_after ON hyper;

--test create trigger on hypertable

-- row triggers: BEFORE
CREATE TRIGGER _0_test_trigger_insert
    BEFORE INSERT ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update
    BEFORE UPDATE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete
    BEFORE delete ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER z_test_trigger_all
    BEFORE INSERT OR UPDATE OR DELETE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

-- row triggers: AFTER
CREATE TRIGGER _0_test_trigger_insert_after
    AFTER INSERT ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update_after
    AFTER UPDATE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete_after
    AFTER delete ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER z_test_trigger_all_after
    AFTER INSERT OR UPDATE OR DELETE ON hyper
    FOR EACH ROW EXECUTE PROCEDURE test_trigger();

-- statement triggers: BEFORE
CREATE TRIGGER _0_test_trigger_insert_s_before
    BEFORE INSERT ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update_s_before
    BEFORE UPDATE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete_s_before
    BEFORE DELETE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

-- statement triggers: AFTER
CREATE TRIGGER _0_test_trigger_insert_s_after
    AFTER INSERT ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_update_s_after
    AFTER UPDATE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();

CREATE TRIGGER _0_test_trigger_delete_s_after
    AFTER DELETE ON hyper
    FOR EACH STATEMENT EXECUTE PROCEDURE test_trigger();


INSERT INTO hyper(time, device_id,sensor_1) VALUES
(1257987600000000000, 'dev1', 1);

INSERT INTO hyper(time, device_id,sensor_1) VALUES
(1257987700000000000, 'dev2', 1), (1257987800000000000, 'dev2', 1);

UPDATE hyper SET sensor_1 = 2;

DELETE FROM hyper;

CREATE TABLE vehicles (
  vehicle_id INTEGER PRIMARY KEY,
  vin_number CHAR(17),
  last_checkup TIMESTAMP
);

CREATE TABLE color (
  color_id INTEGER PRIMARY KEY,
  notes text
);


CREATE TABLE location (
  time TIMESTAMP NOT NULL,
  vehicle_id INTEGER REFERENCES vehicles (vehicle_id),
  color_id INTEGER, --no reference since gonna populate a hypertable
  latitude FLOAT,
  longitude FLOAT
);

CREATE OR REPLACE FUNCTION create_vehicle_trigger_fn()
    RETURNS TRIGGER LANGUAGE PLPGSQL AS
$BODY$
BEGIN
    INSERT INTO vehicles VALUES(NEW.vehicle_id, NULL, NULL) ON CONFLICT DO NOTHING;
    RETURN NEW;
END
$BODY$;


CREATE OR REPLACE FUNCTION create_color_trigger_fn()
    RETURNS TRIGGER LANGUAGE PLPGSQL AS
$BODY$
BEGIN
    --test subtxns within triggers
    BEGIN
        INSERT INTO color VALUES(NEW.color_id, 'n/a');
    EXCEPTION WHEN unique_violation THEN
            -- Nothing to do, just continue
    END;
    RETURN NEW;
END
$BODY$;

CREATE TRIGGER create_color_trigger
    BEFORE INSERT OR UPDATE ON location
    FOR EACH ROW EXECUTE PROCEDURE create_color_trigger_fn();

SELECT create_hypertable('location', 'time');

--make color also a hypertable
SELECT create_hypertable('color', 'color_id', chunk_time_interval=>10);

-- Test that we can create and use triggers with another user
GRANT TRIGGER, INSERT, SELECT, UPDATE ON location TO :ROLE_DEFAULT_PERM_USER_2;
GRANT SELECT, INSERT, UPDATE ON color TO :ROLE_DEFAULT_PERM_USER_2;
GRANT SELECT, INSERT, UPDATE ON vehicles TO :ROLE_DEFAULT_PERM_USER_2;
\c :TEST_DBNAME :ROLE_DEFAULT_PERM_USER_2;

CREATE TRIGGER create_vehicle_trigger
    BEFORE INSERT OR UPDATE ON location
    FOR EACH ROW EXECUTE PROCEDURE create_vehicle_trigger_fn();

INSERT INTO location VALUES('2017-01-01 01:02:03', 1, 1, 40.7493226,-73.9771259);
INSERT INTO location VALUES('2017-01-01 01:02:04', 1, 20, 24.7493226,-73.9771259);
INSERT INTO location VALUES('2017-01-01 01:02:03', 23, 1, 40.7493226,-73.9771269);
INSERT INTO location VALUES('2017-01-01 01:02:03', 53, 20, 40.7493226,-73.9771269);

UPDATE location SET vehicle_id = 52 WHERE vehicle_id = 53;

SELECT * FROM location;
SELECT * FROM vehicles;
SELECT * FROM color;

-- switch back to default user to run some dropping tests
--TODO: currently default user is postgres, who is a superuser, potentially we should change that?
\c :TEST_DBNAME :ROLE_SUPERUSER; 

\set ON_ERROR_STOP 0
-- test that disable trigger is disallowed 
ALTER TABLE location DISABLE TRIGGER create_vehicle_trigger;
\set ON_ERROR_STOP 1

-- test that drop trigger works 
DROP TRIGGER create_color_trigger ON location;
DROP TRIGGER create_vehicle_trigger ON location;

-- test that drop trigger doesn't cause leftovers that mean that dropping chunks or hypertables no longer works
SELECT count(1) FROM pg_depend d WHERE d.classid = 'pg_trigger'::regclass AND NOT EXISTS (SELECT 1 FROM pg_trigger WHERE oid = d.objid);
DROP TABLE location;

