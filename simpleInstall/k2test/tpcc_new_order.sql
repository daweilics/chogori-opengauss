BEGIN;
SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = 1 AND c_w_id = 1 AND c_d_id = 1 AND c_id = 1;
SELECT d_next_o_id, d_tax FROM district WHERE d_id = 1 AND d_w_id = 1;
UPDATE district SET d_next_o_id = 5 WHERE d_id = 1 AND d_w_id = 1;
INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES (5 , 1, 1, 1, '1999-01-08 04:05:06', 1, 1);
INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (5 , 1, 1);
INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (5 , 2, 2);
SELECT i_price, i_name , i_data FROM item WHERE i_id = 2;
SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05 s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 FROM stock WHERE s_i_id = 2 AND s_w_id = 1;
UPDATE stock SET s_quantity = 9 WHERE s_i_id = 2 AND s_w_id = 1;
INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES (5, 1, 1, 1, 2, 1, 1, 12.0, 'a');
COMMIT;
