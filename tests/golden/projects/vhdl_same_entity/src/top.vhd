-- Two entities named core, one per library: units are identified by library and name.
library ieee;
use ieee.std_logic_1164.all;
library alpha, beta;

entity top is
    port (a : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity top;

architecture rtl of top is
    signal t : std_logic_vector(3 downto 0);
begin
    u_inv : entity alpha.core port map (a => a, y => t);
    u_rot : entity beta.core port map (a => t, y => y);
end architecture rtl;
