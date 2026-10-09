-- Library util: clips x to sat_pkg.LIMIT.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library util;
use util.sat_pkg.all;

entity clip is
    port (x : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity clip;

architecture rtl of clip is
begin
    y <= LIMIT when unsigned(x) > unsigned(LIMIT) else x;
end architecture rtl;
