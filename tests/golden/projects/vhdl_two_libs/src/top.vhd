-- Library work: instantiates util.clip and uses util.sat_pkg.
library ieee;
use ieee.std_logic_1164.all;
library util;
use util.sat_pkg.all;

entity top is
    port (x   : in std_logic_vector(3 downto 0);
          y   : out std_logic_vector(3 downto 0);
          hit : out std_logic);
end entity top;

architecture rtl of top is
    signal c : std_logic_vector(3 downto 0);
begin
    u_clip : entity util.clip port map (x => x, y => c);
    y <= c;
    hit <= '1' when c = LIMIT else '0';
end architecture rtl;
