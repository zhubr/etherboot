#!/usr/bin/perl
# Magic Packet for the Web
# Perl version by ken.yap@acm.org after DOS/Windows C version posted by
# Steve_Marfisi@3com.com on the Netboot mailing list
# modified to work with web by guenter.knauf@dialup.soco.de
# Released under GNU Public License
#
use CGI qw(:standard); # import shortcuts
use Socket;

$ver = 'v0.43 &copy; gk 2000-05-20 14:00:00';

# Defaults - Modify to point to the location of your mac file
$www = "$ENV{'DOCUMENT_ROOT'}/perldemo";
$maclist = "$www/wakeup.txt";

MAIN:
{
  # Read in all the variables set by the form
  if (param()) {
    &process_form;
  } else {
    &print_form;
  }
}

sub process_form {
  # Print the header
  print header();
  print start_html("Mp-Form - send Magic Packets");
  # If defined new mac save it
  if (defined(param("mac"))) {
	print h1("Result of adding an entry to the maclist");
	print '<HR><H3><TT>';
	&add_entry;
	print '</TT></H3><HR>';
	print '<FORM method="POST"><input type="submit" value="ok"></FORM>';
  } else {
	# send magic packets to selected macs
	print h1("Result of sending magic packets to multiple PCs");
	print '<HR><H3><TT>';
	if (param("all")) {
		&process_file(S);
	} else {
		for (param()) {
  			&send_broadcast_packet(param($_));
		}
	}
	print '</TT></H3><HR>';
	print '<FORM><input type="button" value="back" onClick="history.back()"></FORM>';
  }
  # Close the document cleanly.
  print end_html;
}

sub print_form {
  # Print the header
  print header();
  print start_html("Mp-Form - send Magic Packets");
  print h1("Form for sending magic packets to multiple PCs");
  print <<ENDOFTEXT;
<HR>
<FORM method="POST">
<H2>Select the destination mac addresses:</H2>
<TABLE BORDER COLS=3 WIDTH="80%">
<TR>
<TD>
<CENTER><B><FONT SIZE="+1">MAC address</FONT></B></CENTER>
</TD>
<TD>
<CENTER><B><FONT SIZE="+1">Name</FONT></B></CENTER>
</TD>
<TD>
<CENTER><B><FONT SIZE="+1">
<input type=checkbox name="all" value="all">Select all
</FONT></B></CENTER>
</TD>
</TR>
ENDOFTEXT
  # build up table with mac addresses
  &process_file;
  # print rest of the form
  print <<ENDOFTEXT;
</TABLE>
<P><B><FONT SIZE="+1">
Press <input type="submit" value="wakeup"> to send the magic packets to your selections.
Press <input type="reset" value="clear"> to reset the form.
</FONT></B>
</FORM>
<HR>
<FORM method="POST">
<H2>Enter new destination mac address:</H2>
<B><FONT SIZE="+1">MAC: </FONT></B><input name="mac" size="17" maxlength="17">
<B><FONT SIZE="+1">Name: </FONT></B><input name="ip" size="40" maxlength="40">
<P><B><FONT SIZE="+1">
Press <input type="submit" value="save"> to add the entry to your maclist.
Press <input type="reset" value="clear"> to reset the form.
</FONT></B>
</FORM>
<HR>
$ver
ENDOFTEXT
  # Close the document cleanly.
  print end_html;
}

sub send_broadcast_packet {
	($mac) = @_;
	if ($mac !~ /^[\da-f]{2}:[\da-f]{2}:[\da-f]{2}:[\da-f]{2}:[\da-f]{2}:[\da-f]{2}$/i)  {
		print "Malformed MAC address $mac<BR>\n";
		return;
	}
	print "Sending wakeup packet to MAC address $mac<BR>\n";
	# Remove colons
	$mac =~ tr/://d;
	# Magic packet is 6 bytes of FF followed by the MAC address 16 times
	$magic = ("\xff" x 6) . (pack('H12', $mac) x 16);
	# Create socket
	socket(S, PF_INET, SOCK_DGRAM, getprotobyname('udp'))
		or die "socket: $!\n";
	# Enable broadcast
	setsockopt(S, SOL_SOCKET, SO_BROADCAST, 1)
		or die "setsockopt: $!\n";
	# Send the wakeup packet
	defined(send(S, $magic, 0, sockaddr_in(0x2fff, INADDR_BROADCAST)))
		or print "send: $!\n";
	close(S);
}

sub process_file {
	unless (open(F, $maclist)) {
		print "Error reading $maclist: $!\n";
	} else {
		while (<F>) {
			next if /^\s*#/;	# skip comments
			($mac, $ip) = split;
			next if (!defined($mac) or $mac eq '');
			if ($_[0] eq 'S') {
			  	&send_broadcast_packet($mac);
			} else {
				print "<TR><TD WIDTH=40%><CENTER><TT>$mac</TT></CENTER></TD>";
				$ip = '&nbsp;' if (!defined($ip) or $ip eq '');
				print "<TD WIDTH=40%><CENTER><TT>$ip</TT></CENTER></TD>";
				print "<TD WIDTH=20%><CENTER><input type=checkbox name=mac$. value=$mac><TT>WakeUp</TT></CENTER></TD></TR>\n";
			}
		}
		close(F);
	}
}

sub add_entry {
	if (param("mac") !~ /^[\da-f]{2}:[\da-f]{2}:[\da-f]{2}:[\da-f]{2}:[\da-f]{2}:[\da-f]{2}$/i) {
		print "Malformed MAC address ",param("mac"),"\n";
		return;
	}
	unless (open(F, ">> $maclist")) {
		print "Error writing $maclist: $!\n";
	} else {
		print F param("mac")." ".param("ip")."\n";
		close(F);
		print "Saved entry to maclist: ".param("mac")." ".param("ip")."\n";
	}
}
