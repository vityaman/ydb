$my_table = (
    SELECT
        1 AS id,
        1 AS ts,
        4 AS value1,
        5 AS value2
    UNION ALL
    SELECT
        3 AS id,
        10 AS ts,
        40 AS value1,
        NULL AS value2
    UNION ALL
    SELECT
        2 AS id,
        1 AS ts,
        NULL AS value1,
        NULL AS value2
    UNION ALL
    SELECT
        1 AS id,
        2 AS ts,
        4 AS value1,
        5 AS value2
    UNION ALL
    SELECT
        3 AS id,
        2 AS ts,
        40 AS value1,
        NULL AS value2
    UNION ALL
    SELECT
        3 AS id,
        5 AS ts,
        2 AS value1,
        7 AS value2
);

-- Эмуляция агрегационной функции COUNT
$cnt_create = ($_item, $_parent) -> {
    RETURN 1ul;
};

$cnt_add = ($state, $_item, $_parent) -> {
    RETURN 1ul + $state;
};

$cnt_merge = ($state1, $state2) -> {
    RETURN $state1 + $state2;
};

$cnt_get_result = ($state) -> {
    RETURN $state;
};

$cnt_serialize = ($state) -> {
    RETURN $state;
};

$cnt_deserialize = ($state) -> {
    RETURN $state;
};

$cnt_default = 0l;

$cnt_udaf_factory = AggregationFactory(
    'UDAF',
    $cnt_create,
    $cnt_add,
    $cnt_merge,
    $cnt_get_result,
    $cnt_serialize,
    $cnt_deserialize,
    $cnt_default
);

SELECT
    AGGREGATE_BY(value1, $cnt_udaf_factory) AS cnt1
FROM
    $my_table
;
