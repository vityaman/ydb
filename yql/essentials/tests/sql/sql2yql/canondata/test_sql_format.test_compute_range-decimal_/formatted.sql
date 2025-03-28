/* syntax version 1 */
/* postgres can not */
/* yt can not */
PRAGMA warning('disable', '4510');

$opt_type = Struct<x: Decimal (15, 10)?>;
$keys = AsTuple(AsAtom('x'));
$pred = ($row) -> (($row.x < Decimal('-inf', 15, 10)) ?? FALSE);

SELECT
    YQL::RangeComputeFor($opt_type, $pred, $keys)
;
