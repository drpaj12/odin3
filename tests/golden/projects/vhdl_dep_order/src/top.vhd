-- Listed first, analysed last: it uses consts_pkg and entity inc.
library ieee;
use ieee.std_logic_1164.all;
use work.consts_pkg.all;

entity top is
    port (x : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity top;

architecture rtl of top is
begin
    u_inc : entity work.inc port map (x => x, y => y);
end architecture rtl;
