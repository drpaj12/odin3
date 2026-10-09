-- The architecture of core, in another file than its entity: analysed after core_ent.vhd
-- (and after util_pkg, which it uses).
library ieee;
use ieee.std_logic_1164.all;
use work.util_pkg.all;

architecture rtl of core is
begin
    y <= swap(x);
end architecture rtl;
