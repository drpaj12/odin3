-- Listed first; needs only entity core to be analysed before it.
library ieee;
use ieee.std_logic_1164.all;

entity top is
    port (x : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity top;

architecture rtl of top is
begin
    u_core : entity work.core port map (x => x, y => y);
end architecture rtl;
